#include "core/engine.h"
#include "core/retry_policy.h"
#include "core/diagnostics.h"
#include <algorithm>
#include <cstring>

namespace fetchkit::core {
namespace {
thread_local const Engine* current_engine=nullptr;
bool resumable(const fetchkit_task_snapshot& s) {
    return s.state==FETCHKIT_PAUSED || s.state==FETCHKIT_RETRY_WAITING ||
        (s.state==FETCHKIT_FAILED && (s.error==FETCHKIT_NETWORK || s.error==FETCHKIT_TIMEOUT));
}
fetchkit_event eventFor(const fetchkit_task_snapshot& s,std::uint32_t kind,std::uint64_t id) {
    return {sizeof(fetchkit_event),kind,id,s.generation,s.state,s.error,s.received_bytes,s.committed_offset,
        s.total_bytes,s.total_known,s.integrity,s.http_status,s.retry_count,s.next_retry_at_ms};
}
void constraints(const DownloadRequest& old,const DownloadRequest& request) {
    if(old.expected_length!=request.expected_length || old.expected_sha256!=request.expected_sha256 ||
        old.require_digest!=request.require_digest || old.reference!=request.reference) throw Failure(FETCHKIT_INVALID_ARGUMENT);
}
std::uint64_t monotonicDeadline(const EngineConfig& config,std::uint64_t due) {
    if(!due) return 0;
    const auto now=config.clock->nowMs(),steady=config.clock->steadyMs();
    const auto remaining=due>now ? due-now : 0;
    return remaining>UINT64_MAX-steady ? UINT64_MAX : steady+remaining;
}
}

void Task::progress(std::uint64_t offset,std::optional<std::uint64_t> total) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        snapshot.received_bytes=offset;snapshot.total_known=total.has_value();snapshot.total_bytes=total.value_or(0);
    }
    if(auto* engine=event_owner.load()) engine->notifyProgress(shared_from_this());
}

Engine::Engine(EngineConfig config,std::shared_ptr<transport::HttpTransport> transport)
    : config_(std::move(config)),transport_(std::move(transport)),
      store_(std::make_shared<storage::ResourceStore>(config_.root,config_.capacity_bytes,config_.staging_retention_ms,config_.clock)),
      random_(config_.random_seed ? config_.random_seed : std::random_device{}()) {
    if(!config_.capacity_bytes || !config_.staging_retention_ms || !config_.max_active || config_.max_active>16 ||
        !config_.max_queued || config_.max_queued>1024) throw Failure(FETCHKIT_INVALID_ARGUMENT);
    try {
        for(std::uint32_t i=0;i<config_.max_active;++i) workers_.emplace_back([this] { loop(false); });
        workers_.emplace_back([this] { loop(true); });
    } catch(...) {
        {std::lock_guard<std::mutex> lock(mutex_);closing_=true;changed_.notify_all();}
        for(auto& worker:workers_) if(worker.joinable()) worker.join();
        throw;
    }
}
Engine::~Engine() { close(); }
bool Engine::onWorker() const { return current_engine==this; }
std::shared_ptr<storage::ResourceStore> Engine::storage() {
    std::lock_guard<std::mutex> lock(mutex_);
    if(closing_ || !store_) throw Failure(FETCHKIT_CLOSED);
    return store_;
}
std::size_t Engine::liveCount(const std::shared_ptr<Task>& task) const {
    std::size_t count=0;
    for(const auto& weak:task->subscribers) if(const auto sub=weak.lock()) if(!sub->inactive && !sub->released) ++count;
    return count;
}
void Engine::queueLocked(const std::shared_ptr<Task>& task) {
    if(!task->queued && !task->processing) {task->queued=true;pending_.push_back(task);}
    changed_.notify_all();
}
void Engine::requestStopLocked(const std::shared_ptr<Task>& task,StopReason reason) {
    if(task->finishing) return;
    if(reason==StopReason::Cancel || task->stop.load()==StopReason::None) task->stop.store(reason);
    task->cancellation->cancel();
    for(const auto& weak:task->subscribers) if(const auto sub=weak.lock()) {
        std::lock_guard<std::mutex> sub_lock(sub->mutex);
        if(sub->provider_cancellation) sub->provider_cancellation->cancel();
    }
    task->snapshot.state=FETCHKIT_PAUSING;
    if(!task->processing) queueLocked(task);
}
void Engine::emitLocked(const std::shared_ptr<Task>& task,std::uint32_t kind) {
    // 锁顺序固定为 engine → task → subscription；此处仅入队，不执行宿主回调或日志输出。
    for(const auto& weak:task->subscribers) if(const auto sub=weak.lock()) {
        if(sub->released || sub->inactive || sub->suppressed) continue;
        auto event=eventFor(task->snapshot,kind,sub->id);
        std::lock_guard<std::mutex> sub_lock(sub->mutex);
        if(kind==FETCHKIT_EVENT_PROGRESS) {
            if(!sub->events.empty() && sub->events.back().kind==FETCHKIT_EVENT_PROGRESS &&
                sub->events.back().generation==event.generation) {sub->events.back()=event;continue;}
            if(sub->events.size()>=64) continue;
        } else if(sub->events.size()>=64) {
            sub->inactive=true;sub->error=FETCHKIT_SUBSCRIPTION_OVERFLOW;sub->events.clear();
            event.kind=FETCHKIT_EVENT_SUBSCRIPTION_ERROR;event.state=FETCHKIT_FAILED;event.error=FETCHKIT_SUBSCRIPTION_OVERFLOW;
        }
        sub->events.push_back(event);
    }
    if(!liveCount(task) && (task->processing || task->queued) && !task->finishing && task->stop.load()==StopReason::None) {
        task->stop.store(StopReason::Pause);task->cancellation->cancel();
        for(const auto& weak:task->subscribers) if(const auto sub=weak.lock()) {
            std::lock_guard<std::mutex> sub_lock(sub->mutex);
            if(sub->provider_cancellation) sub->provider_cancellation->cancel();
        }
        changed_.notify_all();
    }
    task->changed.notify_all();
}
void Engine::notifyProgress(const std::shared_ptr<Task>& task) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::lock_guard<std::mutex> task_lock(task->mutex);
    const auto now=std::chrono::steady_clock::now();
    if(now-task->last_progress<std::chrono::milliseconds(200)) return;
    task->last_progress=now;emitLocked(task,FETCHKIT_EVENT_PROGRESS);
}
void Engine::decorate(const std::shared_ptr<Subscription>& sub,fetchkit_task_snapshot& snapshot) {
    std::shared_ptr<storage::ResourceStore> store;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot.subscriber_count=static_cast<std::uint32_t>(liveCount(sub->task));store=store_;
    }
    if(store) {
        const auto record=store->find(snapshot.persistent_id);
        snapshot.available=record && record->state==FETCHKIT_COMPLETED &&
            (!record->expires_at_ms || record->expires_at_ms>config_.clock->nowMs());
        if(record) {snapshot.pinned=record->pinned;snapshot.expires_at_ms=record->expires_at_ms;}
    }
}
std::optional<fetchkit_task_snapshot> Engine::liveSnapshot(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found=identities_.find(id);if(found==identities_.end()) return {};
    std::lock_guard<std::mutex> task_lock(found->second->mutex);
    auto result=found->second->snapshot;result.task_id=0;result.subscriber_count=static_cast<std::uint32_t>(liveCount(found->second));return result;
}

std::shared_ptr<Subscription> Engine::enqueue(DownloadRequest request,std::uint64_t id,
    const std::function<void(const std::shared_ptr<Subscription>&)>& publish,bool start) {
    if(!request.request.url.empty()) {
        try { const auto url=transport_->resolveUrl("",request.request.url);if(!config_.allow_http && !url.secure) throw Failure(FETCHKIT_INVALID_ARGUMENT); }
        catch(const std::invalid_argument&) {throw Failure(FETCHKIT_INVALID_ARGUMENT);}
    }
    if(request.expected_length && *request.expected_length>config_.max_bytes) throw Failure(FETCHKIT_QUOTA_EXCEEDED);
    const auto resource_id=storage::resourceId(request);
    std::lock_guard<std::mutex> admission(admission_mutex_);
    const auto store=storage();
    std::shared_ptr<Task> task;
    std::shared_ptr<Task> frozen_task;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(closing_) throw Failure(FETCHKIT_CLOSED);
        if(subscriptions_.size()>=1024) throw Failure(FETCHKIT_BUSY);
        const auto found=identities_.find(resource_id);
        if(found!=identities_.end()) task=found->second;
        if(task) {
            std::lock_guard<std::mutex> task_lock(task->mutex);
            constraints(task->request,request);
            if(task->finishing || ((task->processing || task->queued) && task->stop.load()!=StopReason::None)) throw Failure(FETCHKIT_BUSY);
        }
        if(start && (!task || (!task->processing && !task->queued)) && pending_.size()>=config_.max_queued) throw Failure(FETCHKIT_BUSY);
    }
    auto sub=std::make_shared<Subscription>();sub->request=std::move(request);sub->id=id;
    bool prepare=false, execute=false, protected_task=false, new_record=false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(task) {
            std::lock_guard<std::mutex> task_lock(task->mutex);
            std::size_t retained=0;
            for(const auto& weak:task->subscribers) if(const auto existing=weak.lock()) if(!existing->released) ++retained;
            if(retained>=64) throw Failure(FETCHKIT_BUSY);
            if(!task->processing && !task->queued) {task->finishing=true;frozen_task=task;prepare=true;}
        } else {
            for(auto it=identities_.begin();it!=identities_.end();) {
                if(!it->second->processing && !it->second->queued && !liveCount(it->second)) it=identities_.erase(it);else ++it;
            }
            if(identities_.size()>=1024) throw Failure(FETCHKIT_BUSY);
            prepare=true;
        }
    }
    try {
        if(prepare) {
            if(start) {store->protect(resource_id);protected_task=true;}
            auto record=store->find(resource_id);
            if(!record && !start) throw Failure(FETCHKIT_NOT_FOUND);
            if(record) {
                constraints(record->request,sub->request);
                const bool old_expired=record->state==FETCHKIT_COMPLETED && record->expires_at_ms && record->expires_at_ms<=config_.clock->nowMs();
                if(sub->request.expires_at_ms && sub->request.expires_at_ms!=record->expires_at_ms && !old_expired) throw Failure(FETCHKIT_INVALID_ARGUMENT);
            } else {record=store->create(sub->request);new_record=true;}
            const bool expired=record->state==FETCHKIT_COMPLETED && record->expires_at_ms && record->expires_at_ms<=config_.clock->nowMs();
            if(!task || new_record || expired || record->state==FETCHKIT_TASK_CANCELLED || record->state==FETCHKIT_REMOVED) {
                task=std::make_shared<Task>();task->request=sub->request;task->path=store->stagingPath(resource_id);
                task->owner=shared_from_this();task->event_owner=this;task->store=store;task->record=*record;
                task->snapshot.struct_size=sizeof(task->snapshot);task->snapshot.task_id=id;task->snapshot.generation=record->generation;
                task->snapshot.state=record->state;task->snapshot.error=record->error;
                task->snapshot.received_bytes=task->snapshot.committed_offset=record->committed;
                task->snapshot.total_known=record->total.has_value();task->snapshot.total_bytes=record->total.value_or(0);
                task->snapshot.integrity=record->integrity;task->snapshot.retry_count=record->retry_count;
                task->snapshot.next_retry_at_ms=record->next_retry_at_ms;task->snapshot.expires_at_ms=record->expires_at_ms;
                std::memcpy(task->snapshot.persistent_id,resource_id.c_str(),65);
                const auto path=task->path.u8string();std::memcpy(task->snapshot.staging_path,path.c_str(),path.size()+1);
                if(record->state==FETCHKIT_COMPLETED) task->snapshot.staging_path[0]=0;
                task->saved={record->committed,record->total,record->etag};
            }
            task->refresh=start && (expired || record->state==FETCHKIT_TASK_CANCELLED || record->state==FETCHKIT_REMOVED);
            if(task->refresh) {
                task->record.request=sub->request;task->record.expires_at_ms=sub->request.expires_at_ms;
                task->record.request.request={};task->record.request.provider={};
                task->snapshot.next_retry_at_ms=0;task->snapshot.retry_count=0;
            }
            execute=start && (resumable(task->snapshot) || task->refresh);
            if(execute) {
                if(record->pinned && task->refresh) throw Failure(FETCHKIT_BUSY);
                store->protect(resource_id,task->refresh);protected_task=true;
            }
            if(protected_task && !execute) {store->unprotect(resource_id);protected_task=false;}
        }
        if(sub->request.pin_on_create && !task->refresh) store->pin(resource_id,true);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(closing_) throw Failure(FETCHKIT_CLOSED);
            if(frozen_task && frozen_task!=task) {std::lock_guard<std::mutex> frozen_lock(frozen_task->mutex);frozen_task->finishing=false;}
            std::lock_guard<std::mutex> task_lock(task->mutex);
            if(execute && pending_.size()>=config_.max_queued) throw Failure(FETCHKIT_BUSY);
            if(prepare) task->finishing=false;
            if(!task->cancellation) task->cancellation=std::make_shared<transport::Cancellation>();
            sub->task=task;task->subscribers.erase(std::remove_if(task->subscribers.begin(),task->subscribers.end(),[](const auto& weak){
                const auto previous=weak.lock();return !previous || previous->released.load();
            }),task->subscribers.end());
            task->subscribers.push_back(sub);subscriptions_[id]=sub;identities_[resource_id]=task;
            if(execute) {
                if(task->snapshot.generation>=UINT64_MAX-1) throw Failure(FETCHKIT_BUSY);
                // 新记录从 generation 1 开始；恢复/重启时递增，便于宿主识别旧轮次事件。
                if(!new_record) ++task->snapshot.generation;
                task->stop.store(StopReason::None);task->cancellation=std::make_shared<transport::Cancellation>();
                task->snapshot.error=FETCHKIT_OK;task->snapshot.http_status=0;task->snapshot.request_count=0;
                task->snapshot.can_resume=0;
                task->snapshot.state=task->snapshot.next_retry_at_ms>config_.clock->nowMs() ? FETCHKIT_RETRY_WAITING : FETCHKIT_QUEUED;
                task->retry_monotonic_at=std::max(task->retry_monotonic_at,monotonicDeadline(config_,task->snapshot.next_retry_at_ms));
                task->record.auth_refreshes=0;
            }
            if(prepare) {
                task->request.provider=[this,weak=std::weak_ptr<Task>(task)](const ProviderContext& context,RequestData& data,transport::Cancellation& cancellation) {
                    const auto current=weak.lock();if(!current) throw Failure(FETCHKIT_CLOSED);provide(current,context,data,cancellation);
                };
                task->request.request={}; // 凭证仅由订阅持有，不在共享任务或记录中重复保存。
            }
            if(publish) publish(sub);
            if(execute) emitLocked(task);else sub->events.push_back(eventFor(task->snapshot,FETCHKIT_EVENT_STATE,id));
            if(execute) queueLocked(task);
        }
        return sub;
    } catch(...) {
        if(task) {
            std::lock_guard<std::mutex> lock(mutex_);
            if(frozen_task && frozen_task!=task) {std::lock_guard<std::mutex> frozen_lock(frozen_task->mutex);frozen_task->finishing=false;}
            std::lock_guard<std::mutex> task_lock(task->mutex);
            if(prepare) task->finishing=false;
            subscriptions_.erase(id);
            if(!task->processing && !task->queued && task->snapshot.state==FETCHKIT_QUEUED) task->snapshot.state=FETCHKIT_PAUSED;
        }
        if(protected_task) store->unprotect(resource_id);
        throw;
    }
}

void Engine::provide(const std::shared_ptr<Task>& task,const ProviderContext& context,RequestData& data,transport::Cancellation& cancellation) {
    const auto original=data;
    for(unsigned int attempt=0;attempt<64;++attempt) {
        if(cancellation.isCancelled()) throw Failure(FETCHKIT_CANCELLED);
        std::shared_ptr<Subscription> selected;
        auto provider_cancel=std::make_shared<transport::Cancellation>();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for(const auto& weak:task->subscribers) if(const auto sub=weak.lock()) if(!sub->inactive && !sub->released &&
                (!sub->request.request.url.empty() || sub->request.provider)) {selected=sub;break;}
            if(!selected) throw Failure(FETCHKIT_CANCELLED);
            std::lock_guard<std::mutex> sub_lock(selected->mutex);
            ++selected->provider_calls;selected->provider_cancellation=provider_cancel;
            if(task->stop.load()!=StopReason::None) provider_cancel->cancel();
        }
        std::exception_ptr failure;
        try {
            data=context.reason==FETCHKIT_REQUEST_INITIAL ? selected->request.request : original;
            data.origin_before_provider=data.url;
            auto selected_context=context;selected_context.task_id=selected->id;
            if(selected->request.provider) selected->request.provider(selected_context,data,*provider_cancel);
            else if(context.reason==FETCHKIT_REQUEST_AUTH_REFRESH) throw Failure(FETCHKIT_AUTH_REQUIRED);
        } catch(...) {failure=std::current_exception();}
        {
            std::lock_guard<std::mutex> lock(selected->mutex);
            --selected->provider_calls;selected->provider_cancellation.reset();selected->quiescent.notify_all();
        }
        if(selected->inactive || selected->released) continue;
        if(failure) std::rethrow_exception(failure);
        return;
    }
    throw Failure(FETCHKIT_CANCELLED);
}

fetchkit_status Engine::control(const std::shared_ptr<Task>& task,StopReason reason,bool resume) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(closing_) return FETCHKIT_CLOSED;
    std::lock_guard<std::mutex> task_lock(task->mutex);
    const auto current=identities_.find(task->snapshot.persistent_id);
    if(current==identities_.end() || current->second!=task) return FETCHKIT_CLOSED;
    if(task->finishing) return FETCHKIT_BUSY;
    auto& s=task->snapshot;
    if(resume) {
        if(task->processing || task->queued || !resumable(s)) return FETCHKIT_BUSY;
        if(!liveCount(task)) return FETCHKIT_CLOSED;
        bool request_available=false;
        for(const auto& weak:task->subscribers) if(const auto sub=weak.lock()) if(!sub->inactive && !sub->released &&
            (!sub->request.request.url.empty() || sub->request.provider)) request_available=true;
        if(!request_available) return FETCHKIT_INVALID_ARGUMENT;
        if(pending_.size()>=config_.max_queued || s.generation>=UINT64_MAX-1) return FETCHKIT_BUSY;
        try {store_->protect(task->snapshot.persistent_id,false,true);} catch(const Failure& error) {return error.code;}
        task->stop.store(StopReason::None);task->cancellation=std::make_shared<transport::Cancellation>();
        ++s.generation;s.error=FETCHKIT_OK;s.http_status=0;s.platform_code=0;s.request_count=0;s.can_resume=0;
        task->record.auth_refreshes=0;
        s.state=s.next_retry_at_ms>config_.clock->nowMs() ? FETCHKIT_RETRY_WAITING : FETCHKIT_QUEUED;
        task->retry_monotonic_at=s.next_retry_at_ms ? std::max(task->retry_monotonic_at,monotonicDeadline(config_,s.next_retry_at_ms)) : 0;
        queueLocked(task);emitLocked(task);return FETCHKIT_OK;
    }
    if(stoppedState(s.state)) {
        if(s.state==FETCHKIT_PAUSED && reason==StopReason::Pause) return FETCHKIT_OK;
        if(reason!=StopReason::Cancel || s.state==FETCHKIT_COMPLETED || s.state==FETCHKIT_REMOVED) return FETCHKIT_BUSY;
    }
    requestStopLocked(task,reason);emitLocked(task);return FETCHKIT_OK;
}
fetchkit_status Engine::cancel(const std::shared_ptr<Subscription>& sub,fetchkit_status error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(closing_) return FETCHKIT_CLOSED;
    const auto task=sub->task;std::lock_guard<std::mutex> task_lock(task->mutex);
    {
        std::lock_guard<std::mutex> sub_lock(sub->mutex);
        if(sub->released) return FETCHKIT_CLOSED;
        if(sub->inactive) return FETCHKIT_OK;
        sub->inactive=true;sub->error=error;sub->events.clear();
        auto event=eventFor(task->snapshot,error==FETCHKIT_CANCELLED ? FETCHKIT_EVENT_STATE : FETCHKIT_EVENT_SUBSCRIPTION_ERROR,sub->id);
        event.state=error==FETCHKIT_CANCELLED ? FETCHKIT_TASK_CANCELLED : FETCHKIT_FAILED;event.error=error;
        sub->events.push_back(event);
        if(sub->provider_cancellation) sub->provider_cancellation->cancel();
    }
    const auto current=identities_.find(task->snapshot.persistent_id);
    if(current!=identities_.end() && current->second==task && !liveCount(task) && task->snapshot.state!=FETCHKIT_COMPLETED && !task->finishing) {
        requestStopLocked(task,error==FETCHKIT_CANCELLED ? StopReason::Cancel : StopReason::Pause);
    }
    task->changed.notify_all();changed_.notify_all();return FETCHKIT_OK;
}
fetchkit_status Engine::release(const std::shared_ptr<Subscription>& sub) {
    if(onWorker()) return FETCHKIT_BUSY;
    bool wait_task=false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto task=sub->task;std::lock_guard<std::mutex> task_lock(task->mutex);
        {
            std::lock_guard<std::mutex> sub_lock(sub->mutex);
            if(sub->dispatching && sub->dispatch_thread==std::this_thread::get_id()) return FETCHKIT_BUSY;
            sub->released=true;sub->events.clear();
            if(sub->provider_cancellation) sub->provider_cancellation->cancel();
        }
        subscriptions_.erase(sub->id);
        const auto current=identities_.find(task->snapshot.persistent_id);
        if(current!=identities_.end() && current->second==task && !liveCount(task) && (task->processing || task->queued)) {
            requestStopLocked(task,StopReason::Pause);wait_task=true;
        }
    }
    if(wait_task) {
        std::unique_lock<std::mutex> lock(sub->task->mutex);
        sub->task->changed.wait(lock,[&]{return stoppedState(sub->task->snapshot.state);});
    }
    std::unique_lock<std::mutex> sub_lock(sub->mutex);
    sub->quiescent.wait(sub_lock,[&]{return !sub->provider_calls && !sub->dispatching;});
    return FETCHKIT_OK;
}

fetchkit_status Engine::close() {
    if(onWorker()) return FETCHKIT_BUSY;
    std::lock_guard<std::mutex> close_lock(close_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);closing_=true;
        for(const auto& pair:identities_) {
            const auto task=pair.second;std::lock_guard<std::mutex> task_lock(task->mutex);
            if(task->processing || task->queued) requestStopLocked(task,StopReason::Pause);
        }
        for(const auto& pair:subscriptions_) if(const auto sub=pair.second.lock()) {
            sub->suppressed=true;std::lock_guard<std::mutex> sub_lock(sub->mutex);sub->events.clear();
        }
        changed_.notify_all();
    }
    // 等待已进入的磁盘准入事务完成，再释放屏障并等待 provider；持屏障 join 会导致重入死锁。
    {std::lock_guard<std::mutex> admission(admission_mutex_);}
    for(auto& worker:workers_) if(worker.joinable()) worker.join();
    if(store_) store_->shutdown();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for(const auto& pair:identities_) pair.second->event_owner=nullptr;
        for(const auto& pair:subscriptions_) if(const auto sub=pair.second.lock()) sub->task->event_owner=nullptr;
        store_.reset();
    }
    return FETCHKIT_OK;
}

void Engine::loop(bool controls_only) {
    current_engine=this;
    while(true) {
        std::shared_ptr<Task> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            while(!task) {
                if(closing_ && pending_.empty()) {current_engine=nullptr;return;}
                const auto now=config_.clock->nowMs();
                for(auto it=pending_.begin();it!=pending_.end();++it) {
                    const auto candidate=*it;
                    if(candidate->processing || (controls_only && candidate->stop.load()==StopReason::None)) continue;
                    std::lock_guard<std::mutex> task_lock(candidate->mutex);
                    if(candidate->stop.load()==StopReason::None && (candidate->snapshot.next_retry_at_ms>now ||
                        candidate->retry_monotonic_at>config_.clock->steadyMs())) continue;
                    task=candidate;pending_.erase(it);task->queued=false;task->processing=true;
                    task->snapshot.state=task->stop.load()==StopReason::None ? FETCHKIT_RUNNING : FETCHKIT_PAUSING;
                    if(task->stop.load()==StopReason::None) {
                        task->snapshot.next_retry_at_ms=0;task->snapshot.http_status=0;
                        task->snapshot.platform_code=0;task->snapshot.error=FETCHKIT_OK;
                    }
                    emitLocked(task);break;
                }
                if(!task) changed_.wait_for(lock,std::chrono::milliseconds(100));
            }
        }
        run(task);
    }
}

void Engine::run(const std::shared_ptr<Task>& task) {
    RunResult result;
    const char* failure_message = "下载准备失败";
    try {
        store_->protect(task->snapshot.persistent_id);
        if(task->refresh) {
            auto old=task->record;old.generation=task->snapshot.generation;
            task->record=store_->recreate(old);task->saved={0,task->record.total,{}};task->refresh=false;
            task->progress(0,task->record.total);
        }
        task->record.generation=task->snapshot.generation;
        if(task->stop.load()==StopReason::None) {
            task->record.state=FETCHKIT_RUNNING;task->record.error=FETCHKIT_OK;task->record.next_retry_at_ms=0;
            store_->save(task->record);
            store_->reserve(task->record.id,task->saved.offset,task->saved.total);
            task->retry_after_ms=0;
            failure_message = "下载失败";
            result=runDownload(*task,config_,*transport_);
        } else result.error=FETCHKIT_CANCELLED;
        if(result.error==FETCHKIT_OK && task->stop.load()==StopReason::None) {
            {
                std::lock_guard<std::mutex> lock(mutex_);std::lock_guard<std::mutex> task_lock(task->mutex);
                task->snapshot.state=FETCHKIT_VERIFYING;emitLocked(task);
            }
            failure_message = "资源校验失败";
            store_->publish(task->record,*task->cancellation,[&] {
                std::lock_guard<std::mutex> lock(mutex_);std::lock_guard<std::mutex> task_lock(task->mutex);
                if(task->stop.load()!=StopReason::None) throw Failure(FETCHKIT_CANCELLED);
                task->finishing=true;task->snapshot.state=FETCHKIT_PUBLISHING;emitLocked(task);
                failure_message = "资源发布失败";
            });
        }
    } catch(const Failure& error) {result={error.code,error.platform_code};}
    catch(const std::bad_alloc&) {result.error=FETCHKIT_OUT_OF_MEMORY;}
    catch(...) {result.error=FETCHKIT_INTERNAL_ERROR;}
    StopReason stop;
    std::uint64_t jitter=0;
    {
        std::lock_guard<std::mutex> lock(mutex_);std::lock_guard<std::mutex> task_lock(task->mutex);
        task->finishing=true;stop=task->stop.load();jitter=random_();
    }
    std::uint32_t state=FETCHKIT_FAILED;
    bool retry=false;
    try {
        if(stop==StopReason::Cancel) {store_->discard(task->record);result={FETCHKIT_CANCELLED,0};state=FETCHKIT_TASK_CANCELLED;}
        else if(stop==StopReason::Pause && (result.error==FETCHKIT_CANCELLED || result.error==FETCHKIT_OK)) {state=FETCHKIT_PAUSED;result={};}
        else if(result.error==FETCHKIT_OK) state=FETCHKIT_COMPLETED;
        else if(stop==StopReason::None && retryable(result.error,task->snapshot.http_status) && task->record.retry_count<config_.max_retries) {
            const auto now=config_.clock->nowMs();
            const auto delay=retryDelay(config_,task->record.retry_count,jitter);
            task->record.next_retry_at_ms=std::max(task->retry_after_ms,now>static_cast<std::uint64_t>(INT64_MAX)-delay ? std::uint64_t(INT64_MAX) : now+delay);
            ++task->record.retry_count;state=FETCHKIT_RETRY_WAITING;retry=true;
        }
        if(state!=FETCHKIT_COMPLETED && state!=FETCHKIT_TASK_CANCELLED && task->record.state!=FETCHKIT_PUBLISHING && task->record.state!=FETCHKIT_COMPLETED) {
            const auto persisted=store_->find(task->record.id);
            if(persisted) {
                const auto retries=task->record.retry_count;const auto next=task->record.next_retry_at_ms;
                const auto auth=task->record.auth_refreshes;
                task->record=*persisted;task->record.retry_count=retries;task->record.next_retry_at_ms=next;task->record.auth_refreshes=auth;
            }
            task->record.generation=task->snapshot.generation;
            task->record.state=state;task->record.error=result.error;task->record.integrity=FETCHKIT_INTEGRITY_NONE;
            store_->save(task->record);
        }
    } catch(const Failure& error) {
        result={error.code,error.platform_code};state=FETCHKIT_FAILED;retry=false;failure_message="任务收尾或记录提交失败";
    }
    catch(...) {result.error=FETCHKIT_INTERNAL_ERROR;state=FETCHKIT_FAILED;retry=false;failure_message="任务收尾异常";}
    store_->endAttempt(task->record.id);
    if(!retry) store_->unprotect(task->record.id);
    fetchkit_task_snapshot diagnostic{};
    {
        std::lock_guard<std::mutex> lock(mutex_);std::lock_guard<std::mutex> task_lock(task->mutex);
        auto& s=task->snapshot;s.state=state;s.error=result.error;s.platform_code=result.platform_code;
        s.committed_offset=task->record.committed;s.retry_count=task->record.retry_count;s.next_retry_at_ms=task->record.next_retry_at_ms;
        s.integrity=state==FETCHKIT_COMPLETED ? task->record.integrity : FETCHKIT_INTEGRITY_NONE;
        if(state==FETCHKIT_COMPLETED) {s.staging_path[0]=0;s.total_known=1;s.total_bytes=task->record.committed;s.available=1;}
        s.can_resume=resumable(s);task->processing=false;task->finishing=false;
        if(retry) {
            task->retry_monotonic_at=monotonicDeadline(config_,s.next_retry_at_ms);
            if(closing_ || !liveCount(task)) {task->stop.store(StopReason::Pause);task->cancellation->cancel();}
            else {task->cancellation=std::make_shared<transport::Cancellation>();task->stop.store(StopReason::None);}
            queueLocked(task);
        }
        diagnostic=s;
        emitLocked(task);task->changed.notify_all();changed_.notify_all();
    }
    // 每轮只记录一次结果；在释放核心锁后格式化，避免进度路径与日志输出相互阻塞。
    if (retry) logTask(FETCHKIT_LOG_WARN, "下载失败，已安排重试", diagnostic);
    else if (state == FETCHKIT_FAILED) {
        const bool exhausted = config_.max_retries && diagnostic.retry_count >= config_.max_retries &&
            retryable(result.error, diagnostic.http_status);
        logTask(FETCHKIT_LOG_ERROR, exhausted ? "下载重试预算已耗尽" : failure_message, diagnostic);
    } else if (state == FETCHKIT_COMPLETED) logTask(FETCHKIT_LOG_DEBUG, "资源校验与发布完成", diagnostic);
}
}
