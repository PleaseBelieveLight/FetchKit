#include "core/engine.h"

#include <cstring>

namespace fetchkit::core {

void Task::progress(std::uint64_t offset, std::optional<std::uint64_t> total) {
    std::lock_guard<std::mutex> lock(mutex);
    snapshot.received_bytes = offset;
    snapshot.total_known = total.has_value();
    snapshot.total_bytes = total.value_or(0);
}

Engine::Engine(EngineConfig config, std::shared_ptr<transport::HttpTransport> transport)
    : config_(std::move(config)), transport_(std::move(transport)),
      store_(std::make_shared<storage::ResourceStore>(config_.root)) {
    worker_ = std::thread([this] { loop(); });
    worker_id_ = worker_.get_id();
}

Engine::~Engine() { close(); }

std::shared_ptr<Task> Engine::enqueue(DownloadRequest request, std::uint64_t id,
    const std::function<void(const std::shared_ptr<Task>&)>& publish, bool start) {
    if (!request.request.url.empty()) {
        try {
            const auto url = transport_->resolveUrl("", request.request.url);
            if (!config_.allow_http && !url.secure) throw Failure(FETCHKIT_INVALID_ARGUMENT);
        } catch (const std::invalid_argument&) { throw Failure(FETCHKIT_INVALID_ARGUMENT); }
    }
    if (request.expected_length && *request.expected_length > config_.max_bytes) throw Failure(FETCHKIT_QUOTA_EXCEEDED);
    auto task = std::make_shared<Task>();
    task->request = std::move(request);
    task->snapshot.struct_size = sizeof(task->snapshot);
    task->snapshot.task_id = id;
    task->snapshot.generation = 1;
    task->snapshot.state = FETCHKIT_QUEUED;
    task->cancellation = std::make_shared<transport::Cancellation>();
    task->saved.total = task->request.expected_length;
    const auto& r = task->request;
    const auto identity = r.resource_namespace + '\0' + r.resource_key + '\0' + r.version + '\0' + r.representation_key;
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_) throw Failure(FETCHKIT_CLOSED);
    for (auto it = identities_.begin(); it != identities_.end();) {
        if (it->second.expired()) it = identities_.erase(it); else ++it;
    }
    if (identities_.size() >= 1024) throw Failure(FETCHKIT_BUSY);
    if ((start && active_) || !identities_[identity].expired()) throw Failure(FETCHKIT_BUSY);
    const auto persistent_id=storage::resourceId(r);
    auto existing=store_->find(persistent_id);
    if (!existing && !start) throw Failure(FETCHKIT_NOT_FOUND);
    if (existing) {
        const auto& old=existing->request;
        if (old.expected_length!=r.expected_length || old.expected_sha256!=r.expected_sha256 || old.require_digest!=r.require_digest ||
            old.reference!=r.reference) throw Failure(FETCHKIT_INVALID_ARGUMENT);
        task->record=*existing;
        if (task->record.committed>config_.max_bytes || (task->record.total && *task->record.total>config_.max_bytes)) throw Failure(FETCHKIT_QUOTA_EXCEEDED);
    } else task->record=store_->create(r);
    task->saved={task->record.committed,task->record.total,task->record.etag};
    task->snapshot.received_bytes=task->snapshot.committed_offset=task->record.committed;
    task->snapshot.total_known=task->record.total.has_value(); task->snapshot.total_bytes=task->record.total.value_or(0);
    task->snapshot.integrity=task->record.integrity; task->snapshot.error=task->record.error;
    task->snapshot.generation=task->record.generation;
    std::memcpy(task->snapshot.persistent_id,persistent_id.c_str(),65);
    task->snapshot.state=task->record.state;
    task->snapshot.can_resume=task->record.state==FETCHKIT_PAUSED || (task->record.state==FETCHKIT_FAILED &&
        (task->record.error==FETCHKIT_NETWORK || task->record.error==FETCHKIT_TIMEOUT));
    const bool execute=start && task->snapshot.can_resume;
    if (execute) {
        if (existing) {
            if(task->snapshot.generation>=UINT64_MAX-1) throw Failure(FETCHKIT_BUSY);
            ++task->snapshot.generation;
        }
        task->record.generation=task->snapshot.generation;
        task->record.state=FETCHKIT_RUNNING; task->record.error=FETCHKIT_OK; store_->save(task->record);
        task->snapshot.state=FETCHKIT_QUEUED; task->snapshot.error=FETCHKIT_OK; task->snapshot.can_resume=0;
    }
    task->path = store_->stagingPath(persistent_id);
    task->store=store_;
    task->owner = shared_from_this();
    const auto path = task->snapshot.state==FETCHKIT_COMPLETED ? std::string{} : task->path.u8string();
    std::memcpy(task->snapshot.staging_path, path.c_str(), path.size() + 1);
    identities_[identity] = task;
    if (publish) publish(task);
    if (execute) { active_ = pending_ = task; changed_.notify_one(); }
    return task;
}

std::shared_ptr<storage::ResourceStore> Engine::storage() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_ || !store_) throw Failure(FETCHKIT_CLOSED);
    return store_;
}

fetchkit_status Engine::control(const std::shared_ptr<Task>& task, StopReason reason, bool resume) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_) return FETCHKIT_CLOSED;
    std::lock_guard<std::mutex> task_lock(task->mutex);
    auto& snapshot = task->snapshot;
    if (task->finishing) return FETCHKIT_BUSY;
    if (resume) {
        if (task->released) return FETCHKIT_CLOSED;
        if (task->request.request.url.empty() && !task->request.provider) return FETCHKIT_INVALID_ARGUMENT;
        if (active_) return FETCHKIT_BUSY;
        if (snapshot.state != FETCHKIT_PAUSED && !(snapshot.state == FETCHKIT_FAILED && snapshot.can_resume)) return FETCHKIT_BUSY;
        task->cancellation = std::make_shared<transport::Cancellation>();
        task->stop.store(StopReason::None);
        if (snapshot.generation==UINT64_MAX-1) return FETCHKIT_BUSY;
        ++snapshot.generation;
        snapshot.state = FETCHKIT_QUEUED;
        snapshot.error = FETCHKIT_OK;
        snapshot.platform_code = 0;
        snapshot.http_status = 0;
        snapshot.can_resume = 0;
        snapshot.request_count = 0;
        active_ = pending_ = task;
        changed_.notify_one();
        return FETCHKIT_OK;
    }
    if (snapshot.state == FETCHKIT_PAUSED) {
        if (reason == StopReason::Pause) return FETCHKIT_OK;
        if (active_) return FETCHKIT_BUSY;
        task->stop.store(reason);
        task->cancellation->cancel();
        snapshot.state = FETCHKIT_QUEUED;
        active_ = pending_ = task;
        changed_.notify_one();
        return FETCHKIT_OK;
    }
    if (active_ != task || (snapshot.state != FETCHKIT_RUNNING && snapshot.state != FETCHKIT_QUEUED && snapshot.state != FETCHKIT_PAUSING && snapshot.state != FETCHKIT_VERIFYING)) {
        return FETCHKIT_BUSY;
    }
    if (reason == StopReason::Cancel || task->stop.load() == StopReason::None) task->stop.store(reason);
    task->cancellation->cancel();
    return FETCHKIT_OK;
}

fetchkit_status Engine::close() {
    if (onWorker()) return FETCHKIT_BUSY;
    std::lock_guard<std::mutex> close_lock(close_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_ = true;
        if (active_) {
            std::lock_guard<std::mutex> task_lock(active_->mutex);
            if (!active_->finishing) {
                if (active_->stop.load() == StopReason::None) active_->stop.store(StopReason::Pause);
                active_->cancellation->cancel();
            }
        }
        changed_.notify_one();
    }
    if (worker_.joinable()) worker_.join();
    { std::lock_guard<std::mutex> lock(mutex_); store_.reset(); }
    return FETCHKIT_OK;
}

void Engine::loop() {
    while (true) {
        std::shared_ptr<Task> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock, [this] { return closing_ || pending_; });
            if (!pending_) return;
            task = std::move(pending_);
            std::lock_guard<std::mutex> task_lock(task->mutex);
            task->snapshot.state = FETCHKIT_RUNNING;
        }
        RunResult result;
        try { result = runDownload(*task, config_, *transport_); }
        catch (const std::bad_alloc&) { result.error = FETCHKIT_OUT_OF_MEMORY; }
        catch (...) { result.error = FETCHKIT_INTERNAL_ERROR; }
        if (result.error==FETCHKIT_OK && task->stop.load()==StopReason::None) {
            try {
                { std::lock_guard<std::mutex> lock(task->mutex); task->snapshot.state=FETCHKIT_VERIFYING; }
                store_->publish(task->record,*task->cancellation,[&] {
                    std::lock_guard<std::mutex> lock(mutex_);
                    std::lock_guard<std::mutex> task_lock(task->mutex);
                    if (task->stop.load()!=StopReason::None) throw Failure(FETCHKIT_CANCELLED);
                    task->finishing=true;
                    task->snapshot.state=FETCHKIT_PUBLISHING;
                });
            } catch (const Failure& error) { result={error.code,error.platform_code}; }
            catch (const std::bad_alloc&) { result.error=FETCHKIT_OUT_OF_MEMORY; }
            catch (...) { result.error=FETCHKIT_INTERNAL_ERROR; }
        }
        StopReason stop;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::lock_guard<std::mutex> task_lock(task->mutex);
            task->finishing = true;
            stop = task->stop.load();
        }
        std::uint32_t state = FETCHKIT_FAILED;
        if (stop == StopReason::Cancel) {
            try { store_->discard(task->record); result={FETCHKIT_CANCELLED,0}; state=FETCHKIT_TASK_CANCELLED; }
            catch (const Failure& error) { result={error.code,error.platform_code}; }
            catch (...) { result={FETCHKIT_INTERNAL_ERROR,0}; }
        } else if (stop == StopReason::Pause && (result.error == FETCHKIT_CANCELLED || result.error == FETCHKIT_OK)) {
            state = FETCHKIT_PAUSED;
            result = {};
        } else if (result.error == FETCHKIT_OK) {
            state = FETCHKIT_COMPLETED;
        }
        // Never overwrite a durable Publishing record after rename/commit failed.
        // Startup must retain enough evidence to finish that transaction.
        if (state!=FETCHKIT_COMPLETED && state!=FETCHKIT_TASK_CANCELLED &&
            task->record.state!=FETCHKIT_PUBLISHING && task->record.state!=FETCHKIT_COMPLETED) {
            try {
                const auto committed=store_->find(task->record.id);
                if (committed) task->record=*committed;
                task->record.state=state; task->record.error=result.error;
                task->record.integrity=FETCHKIT_INTEGRITY_NONE;
                store_->save(task->record);
            } catch (const Failure& error) { result={error.code,error.platform_code}; state=FETCHKIT_FAILED; }
            catch (...) { result.error=FETCHKIT_INTERNAL_ERROR; state=FETCHKIT_FAILED; }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::lock_guard<std::mutex> task_lock(task->mutex);
            task->snapshot.state = state;
            task->snapshot.error = result.error;
            task->snapshot.platform_code = result.platform_code;
            task->snapshot.committed_offset=task->record.committed;
            task->snapshot.integrity=state==FETCHKIT_COMPLETED ? task->record.integrity : FETCHKIT_INTEGRITY_NONE;
            if (state==FETCHKIT_COMPLETED) {
                task->snapshot.staging_path[0]=0;
                task->snapshot.total_known=1;
                task->snapshot.total_bytes=task->record.committed;
            }
            task->snapshot.can_resume = state == FETCHKIT_PAUSED ||
                (state == FETCHKIT_FAILED && (result.error == FETCHKIT_NETWORK || result.error == FETCHKIT_TIMEOUT));
            active_.reset();
            task->finishing = false;
            task->changed.notify_all();
        }
    }
}

}
