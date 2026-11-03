#include "core/engine.h"
#include "core/retry_policy.h"
#include "core/engine_factory.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <map>

using namespace fetchkit;
using namespace fetchkit::core;

namespace {
void require(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
template<class F> void eventually(F condition,const char* message) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(12);
    while(!condition()) {
        if(std::chrono::steady_clock::now()>=end) throw std::runtime_error(message);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
class ManualClock final : public Clock {
public:
    std::atomic<std::uint64_t> now{1700000000000ULL};
    std::uint64_t nowMs() const override {return now.load();}
};
struct Plan {std::uint64_t size=65536;bool hold=false,unknown=false;unsigned int failures=0;int status=503;std::string retry_after;};
class FakeTransport final : public transport::HttpTransport {
public:
    std::mutex mutex;
    std::map<std::string,Plan> plans;
    std::map<std::string,unsigned int> calls;
    std::vector<transport::Request> requests;
    std::atomic<int> active{0},peak{0};
    std::atomic<bool> gate{false};
    void set(const std::string& url,Plan plan) {std::lock_guard<std::mutex> lock(mutex);plans[url]=std::move(plan);}
    transport::UrlInfo resolveUrl(const std::string&,const std::string& url) override {
        if(url.empty()) throw std::invalid_argument("empty");return {url,"https://fixture:443",true};
    }
    unsigned int count(const std::string& url) {std::lock_guard<std::mutex> lock(mutex);return calls[url];}
    transport::Request lastRequest() {std::lock_guard<std::mutex> lock(mutex);return requests.back();}
    transport::Result execute(const transport::Request& request,transport::Sink& sink,transport::Cancellation& cancellation) override {
        Plan plan;unsigned int call;
        {std::lock_guard<std::mutex> lock(mutex);plan=plans.at(request.url);call=++calls[request.url];requests.push_back(request);}
        const auto running=++active;int previous=peak.load();while(previous<running && !peak.compare_exchange_weak(previous,running)) {}
        struct Exit {std::atomic<int>& active;~Exit(){--active;}} exit{active};
        if(call<=plan.failures) {
            if(plan.status==0) return {transport::EndReason::Network,0};
            std::vector<transport::Header> headers;
            if(!plan.retry_after.empty()) headers.push_back({"Retry-After",plan.retry_after});
            sink.onHeaders({plan.status,headers,request.url},cancellation);return {transport::EndReason::Rejected,0};
        }
        std::uint64_t offset=0;
        for(const auto& header:request.headers) if(header.name=="Range") offset=std::stoull(header.value.substr(6));
        if(offset>=plan.size && offset) {
            sink.onHeaders({416,{{"Content-Range","bytes */"+std::to_string(plan.size)}},request.url},cancellation);
            return {transport::EndReason::Rejected,0};
        }
        std::vector<transport::Header> headers{{"ETag","\"unit-v1\""}};
        if(!plan.unknown) headers.push_back({"Content-Length",std::to_string(plan.size-offset)});
        if(offset) headers.push_back({"Content-Range","bytes "+std::to_string(offset)+"-"+std::to_string(plan.size-1)+"/"+std::to_string(plan.size)});
        if(!sink.onHeaders({offset?206:200,headers,request.url},cancellation)) return {transport::EndReason::Rejected,0};
        std::array<std::uint8_t,1024> bytes{};bytes.fill('x');
        while(offset<plan.size) {
            const auto size=static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(),plan.size-offset));
            if(!sink.onData(bytes.data(),size,cancellation)) return {transport::EndReason::Rejected,0};
            offset+=size;
            if(plan.hold) while(!gate && !cancellation.isCancelled()) cancellation.waitFor(std::chrono::milliseconds(2));
            if(cancellation.isCancelled()) return {transport::EndReason::Cancelled,0};
        }
        return {transport::EndReason::Complete,0};
    }
};
EngineConfig configFor(const std::string& root) {
    EngineConfig c;c.root=root;c.max_bytes=64*1024*1024;c.capacity_bytes=64*1024*1024;
    c.staging_retention_ms=86400000;c.max_retries=0;c.random_seed=17;return c;
}
DownloadRequest request(const std::string& key) {
    DownloadRequest r;r.resource_namespace="m3";r.resource_key=key;r.version="1";r.request.url="https://fixture/"+key;return r;
}
fetchkit_task_snapshot read(const std::shared_ptr<Subscription>& sub) {bool ready;return sub->read(0,ready);}
fetchkit_task_snapshot done(const std::shared_ptr<Subscription>& sub) {
    fetchkit_task_snapshot s;
    eventually([&]{s=read(sub);return stoppedState(s.state);},"task deadline");return s;
}
void completed(const std::shared_ptr<Subscription>& sub) {const auto s=done(sub);require(s.state==FETCHKIT_COMPLETED,"not Completed");}
void countEvent(void* user,const fetchkit_event* event) {static_cast<std::vector<fetchkit_event>*>(user)->push_back(*event);}
void drain(const std::shared_ptr<Subscription>& sub,std::vector<fetchkit_event>& events) {
    std::uint32_t count=0;require(sub->dispatch(countEvent,&events,64,count)==FETCHKIT_OK,"event dispatch");
}

void concurrency(const std::string& root) {
    auto transport=std::make_shared<FakeTransport>();auto cfg=configFor(root);cfg.max_queued=1;
    auto engine=std::make_shared<Engine>(cfg,transport);std::vector<std::shared_ptr<Subscription>> subs;
    for(unsigned int i=0;i<3;++i) {
        auto r=request(std::to_string(i));transport->set(r.request.url,{65536,true});subs.push_back(engine->enqueue(r,i+1));
        eventually([&]{return read(subs.back()).received_bytes>=1024;},"active start");
    }
    auto fourth=request("fourth");transport->set(fourth.request.url,{65536,true});auto queued=engine->enqueue(fourth,4);
    require(read(queued).state==FETCHKIT_QUEUED && transport->count(fourth.request.url)==0,"queue bypassed active bound");
    bool busy=false;try{engine->enqueue(request("overflow"),5);}catch(const Failure& e){busy=e.code==FETCHKIT_BUSY;}require(busy,"queue not bounded");
    require(engine->cancel(queued)==FETCHKIT_OK,"queued cancel");
    eventually([&]{std::lock_guard<std::mutex> lock(queued->task->mutex);return queued->task->snapshot.state==FETCHKIT_TASK_CANCELLED;},"reserved control worker blocked");
    require(transport->peak==3,"default parallelism");transport->gate=true;
    for(const auto& sub:subs) completed(sub);
    engine->close();
}
void sharing(const std::string& root) {
    auto transport=std::make_shared<FakeTransport>();auto r=request("shared");transport->set(r.request.url,{65536,true});
    auto engine=std::make_shared<Engine>(configFor(root),transport);auto first=engine->enqueue(r,1);
    eventually([&]{return read(first).received_bytes>=1024;},"shared start");auto second=engine->enqueue(r,2);
    require(first->task==second->task && read(second).subscriber_count==2,"identity did not share");
    auto conflict=r;conflict.expected_length=17;bool rejected=false;
    try{engine->enqueue(conflict,3);}catch(const Failure& e){rejected=e.code==FETCHKIT_INVALID_ARGUMENT;}require(rejected,"constraint overwrite");
    require(engine->cancel(first)==FETCHKIT_OK && read(first).state==FETCHKIT_TASK_CANCELLED,"subscription cancel");
    require(read(second).state==FETCHKIT_RUNNING,"cancel affected peer");require(engine->release(first)==FETCHKIT_OK,"cancelled release");
    transport->gate=true;completed(second);require(transport->count(r.request.url)==1,"duplicate writer");
    std::vector<fetchkit_event> events;drain(second,events);
    require(std::count_if(events.begin(),events.end(),[](const auto& e){return e.state==FETCHKIT_COMPLETED;})==1,"duplicate terminal event");
    engine->close();
}
void providerLifetime(const std::string& root) {
    auto transport=std::make_shared<FakeTransport>();auto r=request("provider");transport->set(r.request.url,{65536,true});
    std::atomic<bool> entered{false},destroyed{false};std::atomic<int> old_calls{0};
    r.provider=[&](const ProviderContext&,RequestData&,transport::Cancellation& cancel) {
        require(!destroyed,"provider after release");++old_calls;entered=true;
        while(!cancel.isCancelled()) cancel.waitFor(std::chrono::milliseconds(2));throw Failure(FETCHKIT_CANCELLED);
    };
    auto engine=std::make_shared<Engine>(configFor(root),transport);auto first=engine->enqueue(r,1);
    eventually([&]{return entered.load();},"provider not entered");auto peer=r;peer.provider={};auto second=engine->enqueue(peer,2);
    require(engine->release(first)==FETCHKIT_OK,"provider release barrier");destroyed=true;
    eventually([&]{return read(second).received_bytes>=1024;},"provider handoff");transport->gate=true;completed(second);
    require(old_calls==1 && transport->count(r.request.url)==1,"provider reused after release");engine->close();
}
void events(const std::string& root) {
    auto transport=std::make_shared<FakeTransport>();auto r=request("events");transport->set(r.request.url,{65536,true});
    auto engine=std::make_shared<Engine>(configFor(root),transport);auto slow=engine->enqueue(r,1);auto fast=engine->enqueue(r,2);
    std::vector<fetchkit_event> history;
    for(unsigned int i=0;i<25;++i) {
        eventually([&]{return read(fast).received_bytes>=std::uint64_t(i+1)*1024;},"event progress");
        require(engine->control(fast->task,StopReason::Pause)==FETCHKIT_OK,"event pause");require(done(fast).state==FETCHKIT_PAUSED,"event paused");drain(fast,history);
        if(i!=24) require(engine->control(fast->task,StopReason::None,true)==FETCHKIT_OK,"event resume");
    }
    require(read(slow).error==FETCHKIT_SUBSCRIPTION_OVERFLOW && !fast->inactive,"slow observer overflow affected peer");
    std::vector<fetchkit_event> overflow;drain(slow,overflow);require(overflow.size()==1 && overflow[0].kind==FETCHKIT_EVENT_SUBSCRIPTION_ERROR,"overflow final slot");
    for(std::size_t i=1;i<history.size();++i) require(history[i-1].generation<=history[i].generation,"event generation order");
    require(sizeof(fetchkit_event)<=96,"event queue no longer compact");engine->close();
}
struct BlockingObserver {std::mutex mutex;std::condition_variable changed;bool entered=false,allow=false;};
void blockingEvent(void* user,const fetchkit_event*) {
    auto& state=*static_cast<BlockingObserver*>(user);std::unique_lock<std::mutex> lock(state.mutex);
    state.entered=true;state.changed.notify_all();state.changed.wait(lock,[&]{return state.allow;});
}
void slowCallback(const std::string& root) {
    auto transport=std::make_shared<FakeTransport>();auto r=request("callback");transport->set(r.request.url,{65536,true});
    auto engine=std::make_shared<Engine>(configFor(root),transport);auto sub=engine->enqueue(r,1);BlockingObserver observer;
    std::thread executor([&]{std::uint32_t count;sub->dispatch(blockingEvent,&observer,1,count);});
    {std::unique_lock<std::mutex> lock(observer.mutex);require(observer.changed.wait_for(lock,std::chrono::seconds(3),[&]{return observer.entered;}),"observer start");}
    require(engine->control(sub->task,StopReason::Pause)==FETCHKIT_OK,"callback blocked control");require(done(sub).state==FETCHKIT_PAUSED,"callback pause");
    require(engine->close()==FETCHKIT_OK,"close depended on executor");
    {std::lock_guard<std::mutex> lock(observer.mutex);observer.allow=true;observer.changed.notify_all();}executor.join();
    std::uint32_t count;require(sub->dispatch(countEvent,nullptr,1,count)==FETCHKIT_CLOSED,"late callback after close");
}
void retryPersistence(const std::string& root) {
    auto clock=std::make_shared<ManualClock>();auto cfg=configFor(root);cfg.clock=clock;cfg.max_retries=3;cfg.retry_base_ms=10;cfg.retry_max_ms=100;
    auto transport=std::make_shared<FakeTransport>();auto r=request("retry");transport->set(r.request.url,{1024,false,false,1,503,"120"});
    auto engine=std::make_shared<Engine>(cfg,transport);auto sub=engine->enqueue(r,1);
    eventually([&]{return read(sub).state==FETCHKIT_RETRY_WAITING;},"RetryWaiting not reached");
    const auto due=read(sub).next_retry_at_ms;require(due>=clock->now+120000 && read(sub).retry_count==1,"Retry-After shortened");
    engine->close();clock->now+=1000;
    engine=std::make_shared<Engine>(cfg,transport);sub=engine->enqueue(r,2);
    require(read(sub).next_retry_at_ms==due,"restart lost retry deadline");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));require(transport->count(r.request.url)==1,"restart retried early");
    clock->now=due;engine->wake();completed(sub);require(read(sub).retry_count==1 && transport->count(r.request.url)==2,"retry budget reset");engine->close();
}
void retryBudget(const std::string& root) {
    auto clock=std::make_shared<ManualClock>();auto cfg=configFor(root);cfg.clock=clock;cfg.max_retries=3;cfg.retry_base_ms=1;cfg.retry_max_ms=10;
    auto transport=std::make_shared<FakeTransport>();auto r=request("budget");transport->set(r.request.url,{1024,false,false,100,0});
    auto engine=std::make_shared<Engine>(cfg,transport);auto sub=engine->enqueue(r,1);
    for(unsigned int i=1;i<=3;++i) {
        eventually([&]{return read(sub).state==FETCHKIT_RETRY_WAITING && read(sub).retry_count==i;},"retry count");
        clock->now=read(sub).next_retry_at_ms;engine->wake();
    }
    require(done(sub).state==FETCHKIT_FAILED && transport->count(r.request.url)==4,"retry budget unbounded");
    engine->close();engine=std::make_shared<Engine>(cfg,transport);sub=engine->enqueue(r,2);
    require(done(sub).retry_count==3 && transport->count(r.request.url)==5,"restart reset spent budget");engine->close();
}
void authRecovery(const std::string& root) {
    for(const auto status:{401,403}) {
        auto transport=std::make_shared<FakeTransport>();auto old=request("expired");
        old.expected_length=65536;old.require_digest=true;
        old.expected_sha256="1f8745f0d2d1387ec1af2211a3cf417b2e9e885e853472649c1d979d0e9370e3";
        std::atomic<int> refreshes{0};
        old.provider=[&](const ProviderContext& context,RequestData&,transport::Cancellation&) {
            if(context.reason==FETCHKIT_REQUEST_AUTH_REFRESH) ++refreshes;
        };
        transport->set(old.request.url,{65536,true});
        auto engine=std::make_shared<Engine>(configFor(root+'/'+std::to_string(status)),transport);
        auto first=engine->enqueue(old,1);
        eventually([&]{return read(first).received_bytes==1024;},"auth baseline prefix");
        require(engine->control(first->task,StopReason::Pause)==FETCHKIT_OK,"auth baseline pause");
        require(done(first).committed_offset==1024,"auth baseline checkpoint");
        transport->set(old.request.url,{65536,false,false,100,status});
        require(engine->control(first->task,StopReason::None,true)==FETCHKIT_OK,"auth baseline resume");
        const auto failed=done(first);
        require(failed.error==FETCHKIT_AUTH_REQUIRED && failed.committed_offset==1024 && refreshes==1,"auth refresh boundary");
        auto fresh=old;fresh.request.url="https://fixture/refreshed";fresh.provider={};
        transport->set(fresh.request.url,{65536});
        auto second=engine->enqueue(fresh,2);
        const auto rebound=done(second);
        require(rebound.error==FETCHKIT_AUTH_REQUIRED && rebound.generation==failed.generation,"A0 changed: rebind now recovers");
        require(engine->control(second->task,StopReason::None,true)==FETCHKIT_BUSY,"A0 changed: auth failure now resumes");
        require(transport->count(fresh.request.url)==0 && transport->count(old.request.url)==3,"baseline unexpected HTTP request");
        require(engine->storage()->find(failed.persistent_id)->committed==1024,"baseline lost saved prefix");
        auto conflict=fresh;conflict.expected_length=1;bool invalid=false;
        try {engine->retry(second,conflict);} catch(const Failure& error) {invalid=error.code==FETCHKIT_INVALID_ARGUMENT;}
        require(invalid && read(second).generation==failed.generation,"retry conflict mutated task");
        require(engine->retry(second,fresh)==FETCHKIT_OK,"fresh auth request rejected");completed(second);
        const auto restored=read(second);const auto sent=transport->lastRequest();
        require(restored.generation==failed.generation+1 && restored.integrity==FETCHKIT_DIGEST_VERIFIED,"retry generation or digest");
        require(transport->count(fresh.request.url)==1 && transport->count(old.request.url)==3 && transport->peak==1,"retry did not select new source");
        require(std::any_of(sent.headers.begin(),sent.headers.end(),[](const auto& h){return h.name=="Range" && h.value=="bytes=1024-";}),"retry lost exact Range");
        require(std::any_of(sent.headers.begin(),sent.headers.end(),[](const auto& h){return h.name=="If-Range" && h.value=="\"unit-v1\"";}),"retry lost If-Range");
        auto lease=engine->storage()->acquire(restored.persistent_id);std::array<std::uint8_t,65536> bytes{};
        require(lease->read(0,bytes.data(),bytes.size())==bytes.size() && std::all_of(bytes.begin(),bytes.end(),[](auto b){return b=='x';}),"retry lease content");
        require(engine->retry(second,fresh)==FETCHKIT_BUSY,"completed task restarted");
        engine->close();
    }
}
void requestSourceRetry(const std::string& root) {
    auto transport=std::make_shared<FakeTransport>();auto old=request("old-source");
    std::atomic<int> old_calls{0},fresh_calls{0};
    old.provider=[&](const ProviderContext&,RequestData&,transport::Cancellation&) {++old_calls;};
    transport->set(old.request.url,{65536,true});
    auto engine=std::make_shared<Engine>(configFor(root),transport);auto first=engine->enqueue(old,1);
    eventually([&]{return read(first).received_bytes==1024;},"source baseline prefix");
    require(engine->control(first->task,StopReason::Pause)==FETCHKIT_OK,"source baseline pause");
    require(done(first).state==FETCHKIT_PAUSED,"source baseline checkpoint");
    auto fresh=old;fresh.request.url="https://fixture/new-source";
    fresh.provider=[&](const ProviderContext&,RequestData&,transport::Cancellation&) {++fresh_calls;};
    transport->set(fresh.request.url,{65536});transport->gate=true;
    auto second=engine->enqueue(fresh,2,{},false);
    require(engine->retry(second,fresh)==FETCHKIT_OK,"explicit source retry");completed(second);
    require(old_calls==1 && fresh_calls==1,"retry ignored selected provider");
    require(transport->count(old.request.url)==1 && transport->count(fresh.request.url)==1,"retry source selection changed");
    engine->close();
}
void requestRetryBudget(const std::string& root) {
    auto clock=std::make_shared<ManualClock>();auto cfg=configFor(root);cfg.clock=clock;cfg.max_retries=1;
    auto transport=std::make_shared<FakeTransport>();auto old=request("retry-wait");
    transport->set(old.request.url,{65536,false,false,100,503,"120"});
    auto engine=std::make_shared<Engine>(cfg,transport);auto sub=engine->enqueue(old,1);
    eventually([&]{return read(sub).state==FETCHKIT_RETRY_WAITING;},"retry wait not reached");
    require(engine->retry(sub,old)==FETCHKIT_BUSY,"running retry wait replaced");
    const auto due=read(sub).next_retry_at_ms;
    require(engine->control(sub->task,StopReason::Pause)==FETCHKIT_OK,"retry wait pause");done(sub);
    auto fresh=old;fresh.request.url="https://fixture/new-wait";transport->set(fresh.request.url,{65536,false,false,100,0});
    require(engine->retry(sub,fresh)==FETCHKIT_OK,"retry while paused");
    require(read(sub).retry_count==1 && read(sub).next_retry_at_ms==due && transport->count(fresh.request.url)==0,"retry cleared budget or wait");
    clock->now=due;engine->wake();require(done(sub).error==FETCHKIT_NETWORK,"new request failed unexpectedly");
    require(transport->count(fresh.request.url)==1 && read(sub).retry_count==1,"new request obtained extra automatic retries");
    require(engine->retry(sub,fresh)==FETCHKIT_OK,"manual attempt after exhaustion");
    require(done(sub).error==FETCHKIT_NETWORK && transport->count(fresh.request.url)==2 && read(sub).retry_count==1,"manual attempt reset budget");
    engine->close();
}
void requestRetryHandoff(const std::string& root) {
    auto transport=std::make_shared<FakeTransport>();auto old=request("handoff");transport->set(old.request.url,{65536,true});
    auto engine=std::make_shared<Engine>(configFor(root),transport);auto first=engine->enqueue(old,1);
    eventually([&]{return read(first).received_bytes==1024;},"handoff initial prefix");
    require(engine->control(first->task,StopReason::Pause)==FETCHKIT_OK,"handoff pause");done(first);
    std::atomic<bool> entered{false},retired{false};std::atomic<int> calls{0};
    auto fresh=old;fresh.provider=[&](const ProviderContext&,RequestData&,transport::Cancellation& cancel) {
        require(!retired,"released retry provider accessed");++calls;entered=true;
        while(!cancel.isCancelled()) cancel.waitFor(std::chrono::milliseconds(2));throw Failure(FETCHKIT_CANCELLED);
    };
    auto second=engine->enqueue(fresh,2,{},false);
    require(engine->retry(second,fresh)==FETCHKIT_OK,"handoff retry");eventually([&]{return entered.load();},"selected retry provider not entered");
    transport->gate=true;require(engine->release(second)==FETCHKIT_OK,"retry provider release barrier");retired=true;
    completed(first);require(calls==1,"retired provider used again");
    require(engine->retry(second,old)==FETCHKIT_CLOSED,"released retry handle accepted");engine->close();
}
void requestRetryAdmission(const std::string& root) {
    auto cfg=configFor(root);cfg.max_active=1;cfg.max_queued=1;
    auto transport=std::make_shared<FakeTransport>();auto old=request("admission");transport->set(old.request.url,{65536,true});
    auto engine=std::make_shared<Engine>(cfg,transport);auto sub=engine->enqueue(old,1);
    eventually([&]{return read(sub).received_bytes==1024;},"admission prefix");
    require(engine->retry(sub,old)==FETCHKIT_BUSY,"active retry accepted");
    require(engine->control(sub->task,StopReason::Pause)==FETCHKIT_OK,"admission pause");const auto paused=done(sub);
    auto fresh=old;fresh.request.url="https://fixture/admission-new";transport->set(fresh.request.url,{65536});
    auto wrong=fresh;wrong.resource_key="wrong";require(engine->retry(sub,wrong)==FETCHKIT_INVALID_ARGUMENT,"identity replacement accepted");
    wrong=fresh;wrong.expires_at_ms=1;require(engine->retry(sub,wrong)==FETCHKIT_INVALID_ARGUMENT,"expiry overwritten by retry");
    wrong=fresh;wrong.request.url.clear();require(engine->retry(sub,wrong)==FETCHKIT_INVALID_ARGUMENT,"empty request accepted");
    auto active=request("blocker");transport->set(active.request.url,{65536,true});auto blocker=engine->enqueue(active,2);
    eventually([&]{return read(blocker).received_bytes==1024;},"admission blocker");
    auto pending=request("pending");transport->set(pending.request.url,{65536});auto queued=engine->enqueue(pending,3);
    require(engine->retry(sub,fresh)==FETCHKIT_BUSY,"queue full retry accepted");
    require(read(sub).generation==paused.generation && sub->request.request.url==old.request.url && read(sub).committed_offset==1024,"failed admission mutated old request");
    require(engine->cancel(queued)==FETCHKIT_OK,"admission cancel pending");
    eventually([&]{const auto status=engine->retry(sub,fresh);require(status==FETCHKIT_OK || status==FETCHKIT_BUSY,"retry queue admission");return status==FETCHKIT_OK;},"admission pending cleanup");
    require(engine->retry(sub,fresh)==FETCHKIT_BUSY,"queued retry accepted twice");
    require(engine->cancel(sub)==FETCHKIT_OK && engine->retry(sub,old)==FETCHKIT_CANCELLED,"cancelled retry accepted");
    engine->close();
}
void requestRetryCloseRace(const std::string& root) {
    for(int round=0;round<6;++round) {
        auto transport=std::make_shared<FakeTransport>();auto r=request("race");transport->set(r.request.url,{65536,true});
        auto engine=std::make_shared<Engine>(configFor(root+'/'+std::to_string(round)),transport);auto sub=engine->enqueue(r,1);
        eventually([&]{return read(sub).received_bytes==1024;},"retry race prefix");
        require(engine->control(sub->task,StopReason::Pause)==FETCHKIT_OK,"retry race pause");done(sub);
        std::atomic<bool> begin{false};fetchkit_status result=FETCHKIT_INTERNAL_ERROR;
        std::thread retry([&]{while(!begin) std::this_thread::yield();try {result=engine->retry(sub,r);} catch(const Failure& e) {result=e.code;}});
        begin=true;require(engine->close()==FETCHKIT_OK,"retry close race");retry.join();
        require(result==FETCHKIT_OK || result==FETCHKIT_CLOSED,"unexpected retry/close result");
        require(transport->active==0,"retry I/O survived close");
    }
}
struct ProviderFailure {std::atomic<int> calls{0};fetchkit_status status=FETCHKIT_NETWORK;};
fetchkit_status transientProvider(void* raw,const fetchkit_request_context*,fetchkit_request_builder*) {
    auto& state=*static_cast<ProviderFailure*>(raw);return ++state.calls==1 ? state.status : FETCHKIT_OK;
}
void directTaskQuery(const std::string& root) {
    auto transport = std::make_shared<FakeTransport>();
    fetchkit_engine_config config;
    fetchkit_engine_config_init(&config);
    config.storage_root = {root.data(), root.size()};
    config.max_download_bytes = 65536;
    config.storage_capacity_bytes = 16 * 1024 * 1024;
    config.staging_retention_ms = 86400000;
    const auto view = [](const std::string& value) { return fetchkit_string{value.data(), value.size()}; };
    const std::string url = "https://fixture/query";
    const std::string key = u8"资源-🚀";
    transport->set(url, {65536});
    fetchkit_download_request request;
    fetchkit_download_request_init(&request);
    const std::string resource_namespace = "query", version = "1";
    request.resource_namespace = view(resource_namespace);
    request.resource_key = view(key);
    request.version = view(version);
    request.url = view(url);
    fetchkit_engine engine = 0;
    require(createEngineWithTransport(&config, &engine, transport) == FETCHKIT_OK, "query fixture open");
    fetchkit_task_info info{};
    info.struct_size = sizeof(info);
    const auto queryIdentity = [&] {
        return fetchkit_query_task_by_identity(engine, request.resource_namespace, request.resource_key,
            request.version, request.representation_key, &info);
    };
    require(queryIdentity() == FETCHKIT_NOT_FOUND, "missing identity");
    require(fetchkit_query_task_by_id(engine, view("../bad"), &info) == FETCHKIT_INVALID_ARGUMENT, "invalid id");
    fetchkit_task sub = 0;
    require(fetchkit_enqueue(engine, &request, &sub) == FETCHKIT_OK, "query fixture enqueue");
    fetchkit_task_snapshot done{};
    done.struct_size = sizeof(done);
    require(fetchkit_task_wait(sub, 10000, &done) == FETCHKIT_OK && done.state == FETCHKIT_COMPLETED, "query fixture completion");
    require(queryIdentity() == FETCHKIT_OK && info.snapshot.task_id == 0 && info.snapshot.available &&
        info.snapshot.generation == done.generation && info.resource_key == key, "identity snapshot");
    const std::string id = info.snapshot.persistent_id;
    require(fetchkit_task_release(sub) == FETCHKIT_OK, "query fixture release");
    fetchkit_status close_query = FETCHKIT_INTERNAL_ERROR;
    std::thread closing_reader([&] { close_query = queryIdentity(); });
    const auto closed = fetchkit_engine_close(engine);
    closing_reader.join();
    require(closed == FETCHKIT_OK && (close_query == FETCHKIT_OK || close_query == FETCHKIT_CLOSED), "query close race");
    require(fetchkit_query_task_by_id(engine, view(id), &info) == FETCHKIT_CLOSED, "closed query");
    require(createEngineWithTransport(&config, &engine, transport) == FETCHKIT_OK, "query fixture reopen");
    require(fetchkit_query_task_by_id(engine, view(id), &info) == FETCHKIT_OK && info.snapshot.subscriber_count == 0 &&
        info.snapshot.generation == done.generation && transport->count(url) == 1, "reopen query started work");
    require(fetchkit_resource_set_expiry(engine, view(id), 1) == FETCHKIT_OK &&
        queryIdentity() == FETCHKIT_OK && !info.snapshot.available, "expired query availability");
    fetchkit_status concurrent = FETCHKIT_INTERNAL_ERROR;
    std::thread reader([&] { concurrent = queryIdentity(); });
    const auto removed = fetchkit_resource_remove(engine, view(id));
    reader.join();
    require(removed == FETCHKIT_OK && (concurrent == FETCHKIT_OK || concurrent == FETCHKIT_NOT_FOUND) &&
        queryIdentity() == FETCHKIT_NOT_FOUND, "query removal race");
    require(fetchkit_enqueue(engine, &request, &sub) == FETCHKIT_OK, "query fixture recreate");
    require(fetchkit_task_wait(sub, 10000, &done) == FETCHKIT_OK && done.state == FETCHKIT_COMPLETED, "query fixture republish");
    require(fetchkit_task_release(sub) == FETCHKIT_OK, "query fixture release after republish");
    std::thread trim_reader([&] { concurrent = queryIdentity(); });
    const auto trimmed = fetchkit_storage_trim(engine, 0);
    trim_reader.join();
    require(trimmed == FETCHKIT_OK && (concurrent == FETCHKIT_OK || concurrent == FETCHKIT_NOT_FOUND) &&
        queryIdentity() == FETCHKIT_NOT_FOUND, "query trim race");
    require(fetchkit_engine_close(engine) == FETCHKIT_OK, "query fixture final close");
}
void requestPolicyCAbi(const std::string& root) {
    auto transport=std::make_shared<FakeTransport>();fetchkit_engine_config c;fetchkit_engine_config_init(&c);
    c.storage_root={root.data(),root.size()};c.max_download_bytes=65536;c.storage_capacity_bytes=16*1024*1024;c.staging_retention_ms=86400000;
    c.max_retries=1;c.retry_base_delay_ms=1;c.retry_max_delay_ms=1;
    fetchkit_engine engine=0;require(createEngineWithTransport(&c,&engine,transport)==FETCHKIT_OK,"C API fixture open");
    const auto view=[](const std::string& s){return fetchkit_string{s.data(),s.size()};};
    for(const auto status:{FETCHKIT_NETWORK,FETCHKIT_TIMEOUT,FETCHKIT_AUTH_REQUIRED,FETCHKIT_CANCELLED,FETCHKIT_INTERNAL_ERROR}) {
        const auto key=std::to_string(status),url="https://fixture/provider-"+key;
        transport->set(url,{65536});fetchkit_download_request r;fetchkit_download_request_init(&r);
        r.resource_namespace=view(root);r.resource_key=view(key);r.version=view(key);r.url=view(url);
        ProviderFailure failure;failure.status=status;r.provider=transientProvider;r.provider_user_data=&failure;
        fetchkit_task sub=0;require(fetchkit_enqueue(engine,&r,&sub)==FETCHKIT_OK,"C API provider enqueue");
        fetchkit_task_snapshot s{};s.struct_size=sizeof(s);require(fetchkit_task_wait(sub,10000,&s)==FETCHKIT_OK,"C API provider wait");
        if(status==FETCHKIT_NETWORK || status==FETCHKIT_TIMEOUT) require(s.state==FETCHKIT_COMPLETED && s.retry_count==1,"typed provider transient not retried");
        else require(s.error==(status==FETCHKIT_CANCELLED ? FETCHKIT_CANCELLED : FETCHKIT_AUTH_REQUIRED),"provider compatibility mapping");
        require(fetchkit_task_release(sub)==FETCHKIT_OK,"provider release");
    }
    for(const auto status:{404,410}) for(const auto enabled:{false,true}) {
        const auto key=std::to_string(status)+(enabled ? "-enabled" : "-default"),url="https://fixture/"+key;
        transport->set(url,{65536,false,false,1,status});fetchkit_download_request r;fetchkit_download_request_init(&r);
        r.resource_namespace=view(root);r.resource_key=view(key);r.version=view(key);r.url=view(url);
        ProviderFailure failure;failure.calls=1;r.provider=transientProvider;r.provider_user_data=&failure;
        fetchkit_request_options options;fetchkit_request_options_init(&options);
        if(enabled) {options.refresh_http_status_count=1;options.refresh_http_statuses[0]=status;}
        fetchkit_task sub=0;require(fetchkit_enqueue_with_options(engine,&r,&options,&sub)==FETCHKIT_OK,"refresh policy enqueue");
        fetchkit_task_snapshot s{};s.struct_size=sizeof(s);require(fetchkit_task_wait(sub,10000,&s)==FETCHKIT_OK,"refresh policy wait");
        require(enabled ? s.state==FETCHKIT_COMPLETED : s.error==FETCHKIT_HTTP_STATUS,"refresh opt-in classification");
        if(!enabled) {require(fetchkit_task_retry(sub,&r,nullptr)==FETCHKIT_OK,"explicit HTTP failure retry");
            require(fetchkit_task_wait(sub,10000,&s)==FETCHKIT_OK && s.state==FETCHKIT_COMPLETED,"explicit HTTP failure recovery");}
        require(fetchkit_task_release(sub)==FETCHKIT_OK,"refresh policy release");
        options.refresh_http_status_count=2;options.refresh_http_statuses[0]=404;options.refresh_http_statuses[1]=404;
        require(fetchkit_enqueue_with_options(engine,&r,&options,&sub)==FETCHKIT_INVALID_ARGUMENT && sub==0,"duplicate refresh status accepted");
    }
    require(fetchkit_engine_close(engine)==FETCHKIT_OK,"C API fixture close");
}
void quota(const std::string& root) {
    auto cfg=configFor(root);cfg.capacity_bytes=storage::ResourceStore::metadataBudget()*3+131072;
    auto transport=std::make_shared<FakeTransport>();auto engine=std::make_shared<Engine>(cfg,transport);
    std::vector<std::shared_ptr<Subscription>> active;
    for(unsigned int i=0;i<2;++i) {
        auto r=request("known"+std::to_string(i));r.expected_length=65536;transport->set(r.request.url,{65536,true});active.push_back(engine->enqueue(r,i+1));
        eventually([&]{return read(active.back()).received_bytes>=1024;},"quota start");
    }
    auto r=request("no-space");r.expected_length=65536;transport->set(r.request.url,{65536,true});auto failed=engine->enqueue(r,3);
    require(done(failed).error==FETCHKIT_QUOTA_EXCEEDED,"concurrent quota oversold");
    const auto stats=engine->storage()->stats();require(stats.charged_bytes<=stats.capacity_bytes && stats.reserved_bytes>0,"reservation ledger");
    bool admission_failed=false;
    try {engine->enqueue(request("admission-overflow"),4);}catch(const Failure& e){admission_failed=e.code==FETCHKIT_QUOTA_EXCEEDED;}
    require(admission_failed && engine->storage()->stats().charged_bytes==stats.charged_bytes,"admission failure lost active reservations");
    transport->gate=true;for(const auto& sub:active) completed(sub);engine->close();
}
void unknownQuota(const std::string& root) {
    auto cfg=configFor(root);cfg.capacity_bytes=storage::ResourceStore::metadataBudget()+65536;
    auto transport=std::make_shared<FakeTransport>();auto r=request("unknown");transport->set(r.request.url,{131072,false,true});
    auto engine=std::make_shared<Engine>(cfg,transport);auto sub=engine->enqueue(r,1);const auto s=done(sub);
    require(s.error==FETCHKIT_QUOTA_EXCEEDED && s.committed_offset==65536 && !s.available,"unknown-length quota");
    const auto stats=engine->storage()->stats();require(stats.charged_bytes==cfg.capacity_bytes,"unknown quota accounting");engine->close();
}
void cache(const std::string& root) {
    auto clock=std::make_shared<ManualClock>();auto cfg=configFor(root);cfg.clock=clock;
    cfg.capacity_bytes=storage::ResourceStore::metadataBudget()*2+16384;
    auto transport=std::make_shared<FakeTransport>();auto engine=std::make_shared<Engine>(cfg,transport);
    std::vector<std::shared_ptr<Subscription>> subs;
    auto add=[&](const std::string& key) {
        auto r=request(key);r.expected_length=8192;transport->set(r.request.url,{8192});auto sub=engine->enqueue(r,subs.size()+1);completed(sub);subs.push_back(sub);return std::string(read(sub).persistent_id);
    };
    const auto a=add("a"),b=add("b");auto store=engine->storage();{auto lease=store->acquire(a);}
    const auto c=add("c");require(store->available(a) && !store->available(b) && store->available(c),"LRU order");
    store->pin(a,true);bool blocked=false;try{store->trimTo(0);}catch(const Failure& e){blocked=e.code==FETCHKIT_QUOTA_EXCEEDED;}
    require(blocked && store->available(a),"pin evicted");auto lease=store->acquire(a);store->pin(a,false);
    blocked=false;try{store->remove(a);}catch(const Failure& e){blocked=e.code==FETCHKIT_BUSY;}require(blocked,"leased object removed");
    store->setExpiry(a,clock->now-1);blocked=false;try{store->acquire(a);}catch(const Failure& e){blocked=e.code==FETCHKIT_NOT_FOUND;}require(blocked,"expired acquire");
    std::array<std::uint8_t,16> bytes{};require(lease->read(0,bytes.data(),bytes.size())==16 && bytes[0]=='x',"expired existing lease");
    lease.reset();store->trimTo(0);require(store->stats().charged_bytes==0,"trim retained record budget");engine->close();
}
void retention(const std::string& root) {
    auto clock=std::make_shared<ManualClock>();auto cfg=configFor(root);cfg.clock=clock;cfg.staging_retention_ms=1000;
    auto transport=std::make_shared<FakeTransport>();auto r=request("staging");transport->set(r.request.url,{65536,true});
    auto engine=std::make_shared<Engine>(cfg,transport);auto sub=engine->enqueue(r,1);
    eventually([&]{return read(sub).received_bytes>=1024;},"retention start");
    clock->now+=2000;engine->storage()->collectStaging();require(engine->storage()->find(read(sub).persistent_id).has_value(),"active staging collected");
    require(engine->control(sub->task,StopReason::Pause)==FETCHKIT_OK && done(sub).state==FETCHKIT_PAUSED,"retention pause");
    const auto before=engine->storage()->stats().charged_bytes;engine->close();
    clock->now+=1001;
    engine=std::make_shared<Engine>(cfg,transport);require(engine->storage()->stats().charged_bytes==before,"startup double charge");
    engine->storage()->collectStaging();require(engine->storage()->stats().record_count==0,"startup reset staging age");engine->close();
}
void dates(const std::string&) {
    constexpr std::uint64_t now=784111777000ULL;
    require(retryAfter("Sun, 06 Nov 1994 08:49:37 GMT",0)==now,"IMF date");
    require(retryAfter("Sunday, 06-Nov-94 08:49:37 GMT",now)==now,"obsolete date");
    require(retryAfter("Sun Nov  6 08:49:37 1994",0)==now,"asctime date");
    require(retryAfter("120",now)==now+120000 && retryAfter("-1",now)==0,"delay-seconds");
    require(retryAfter("999999999999999999999999",now)==INT64_MAX,"Retry-After overflow");
    auto cfg=configFor("");cfg.retry_base_ms=1000;cfg.retry_max_ms=30000;
    require(retryDelay(cfg,0,0)==1000 && retryDelay(cfg,0,250)==1250 && retryDelay(cfg,16,99999)==30000,"jitter bound");
    require(!retryable(FETCHKIT_INTEGRITY_MISMATCH,0) && !retryable(FETCHKIT_CERTIFICATE_ERROR,0) && retryable(FETCHKIT_HTTP_STATUS,429),"retry classification");
}
void expiredRefresh(const std::string& root) {
    auto clock=std::make_shared<ManualClock>();auto cfg=configFor(root);cfg.clock=clock;
    auto transport=std::make_shared<FakeTransport>();auto r=request("expired");r.pin_on_create=true;transport->set(r.request.url,{8192});
    auto engine=std::make_shared<Engine>(cfg,transport);auto first=engine->enqueue(r,1);completed(first);
    const auto id=std::string(read(first).persistent_id);auto store=engine->storage();
    require(store->find(id)->pinned,"pin-on-create not atomic");store->setExpiry(id,clock->now-1);
    bool busy=false;try{engine->enqueue(r,2);}catch(const Failure& e){busy=e.code==FETCHKIT_BUSY;}require(busy,"expired pin replaced");
    store->pin(id,false);auto refreshed=r;refreshed.expires_at_ms=clock->now+100000;
    auto second=engine->enqueue(refreshed,3);completed(second);
    require(read(second).generation==2 && read(second).available && transport->count(r.request.url)==2,"expired refresh did not redownload");
    require(store->find(id)->pinned && store->find(id)->expires_at_ms==refreshed.expires_at_ms,"refresh retention policy lost");engine->close();
}
void subscriberLimit(const std::string& root) {
    auto transport=std::make_shared<FakeTransport>();auto r=request("limit");transport->set(r.request.url,{65536,true});
    auto engine=std::make_shared<Engine>(configFor(root),transport);std::vector<std::shared_ptr<Subscription>> subs;
    for(unsigned int i=1;i<=64;++i) subs.push_back(engine->enqueue(r,i));
    bool busy=false;try{engine->enqueue(r,65);}catch(const Failure& e){busy=e.code==FETCHKIT_BUSY;}require(busy,"subscription bound");
    engine->cancel(subs.front());busy=false;try{engine->enqueue(r,66);}catch(const Failure& e){busy=e.code==FETCHKIT_BUSY;}
    require(busy,"unreleased cancelled handle not charged");engine->release(subs.front());auto peer=engine->enqueue(r,67);
    require(peer->task==subs.back()->task,"released subscription slot not reusable");transport->gate=true;completed(peer);engine->close();
}
struct ReentrantState {std::shared_ptr<Engine> engine;std::shared_ptr<Subscription> sub;bool called=false;};
void reentrantEvent(void* user,const fetchkit_event*) {
    auto& state=*static_cast<ReentrantState*>(user);state.called=true;
    require(state.engine->release(state.sub)==FETCHKIT_BUSY,"self-release joined callback");
    require(state.engine->control(state.sub->task,StopReason::Pause)==FETCHKIT_OK,"callback reentry control");
    require(state.engine->close()==FETCHKIT_OK,"callback reentry close");
}
void reentrancy(const std::string& root) {
    auto transport=std::make_shared<FakeTransport>();auto r=request("reentrant");transport->set(r.request.url,{65536,true});
    auto engine=std::make_shared<Engine>(configFor(root),transport);auto sub=engine->enqueue(r,1);ReentrantState state{engine,sub};
    std::uint32_t count=0;require(sub->dispatch(reentrantEvent,&state,1,count)==FETCHKIT_OK && state.called && count==1,"callback reentry");
    require(sub->dispatch(reentrantEvent,&state,1,count)==FETCHKIT_CLOSED,"closed executor delivered late event");
}
void largeLedger(const std::string& root) {
    auto clock=std::make_shared<ManualClock>();auto store=std::make_shared<storage::ResourceStore>(root,8ULL*1024*1024*1024,1000,clock);
    auto r=request("large-ledger");r.expected_length=4295032835ULL;const auto record=store->create(r);
    store->protect(record.id);store->reserve(record.id,0,r.expected_length);
    const auto stats=store->stats();require(stats.charged_bytes==*r.expected_length+storage::ResourceStore::metadataBudget() && stats.reserved_bytes>UINT32_MAX,"64-bit quota reservation");
    store->endAttempt(record.id);store->unprotect(record.id);require(store->stats().charged_bytes==storage::ResourceStore::metadataBudget(),"reservation rollback");
}
void closeRaces(const std::string& root) {
    for(unsigned int round=0;round<20;++round) {
        auto cfg=configFor(root+"/"+std::to_string(round));auto transport=std::make_shared<FakeTransport>();
        auto r=request("race");transport->set(r.request.url,{65536,true});
        auto engine=std::make_shared<Engine>(cfg,transport);auto a=engine->enqueue(r,1);auto b=engine->enqueue(r,2);
        eventually([&]{return read(b).received_bytes>=1024;},"race start");
        std::thread cancel([&]{engine->cancel(a);});
        std::thread pause([&]{engine->control(b->task,StopReason::Pause);});
        std::thread release([&]{engine->release(b);});
        std::thread close([&]{engine->close();});
        cancel.join();pause.join();release.join();close.join();
        require(transport->active==0,"I/O after close");
        {
            std::lock_guard<std::mutex> lock(a->task->mutex);
            require(stoppedState(a->task->snapshot.state),"close left active state");
        }
        auto reopened=std::make_shared<Engine>(cfg,transport);
        require(reopened->storage()->stats().charged_bytes<=cfg.capacity_bytes,"close leaked reservation");reopened->close();
    }
}
void staleSubscription(const std::string& root) {
    auto clock=std::make_shared<ManualClock>();auto cfg=configFor(root);cfg.clock=clock;cfg.staging_retention_ms=1000;
    auto transport=std::make_shared<FakeTransport>();auto r=request("stale");transport->set(r.request.url,{65536,true});
    auto engine=std::make_shared<Engine>(cfg,transport);auto old=engine->enqueue(r,1);
    eventually([&]{return read(old).received_bytes>=1024;},"stale start");engine->control(old->task,StopReason::Pause);
    require(done(old).state==FETCHKIT_PAUSED,"stale pause");clock->now+=1001;engine->storage()->collectStaging();
    require(engine->control(old->task,StopReason::None,true)==FETCHKIT_NOT_FOUND,"missing checkpoint resumed");
    auto current=engine->enqueue(r,2);eventually([&]{return read(current).received_bytes>=1024;},"replacement start");
    require(engine->control(old->task,StopReason::Pause)==FETCHKIT_CLOSED,"obsolete task controlled replacement");
    require(engine->cancel(old)==FETCHKIT_OK && read(current).state==FETCHKIT_RUNNING,"obsolete cancel stopped replacement");
    transport->gate=true;completed(current);engine->close();
}
void queuedGeneration(const std::string& root) {
    auto cfg=configFor(root);cfg.max_active=1;auto transport=std::make_shared<FakeTransport>();
    auto block=request("block");transport->set(block.request.url,{65536,true});auto queued=request("queued");transport->set(queued.request.url,{65536,true});
    auto engine=std::make_shared<Engine>(cfg,transport);auto active=engine->enqueue(block,1);
    eventually([&]{return read(active).received_bytes>=1024;},"generation active");auto sub=engine->enqueue(queued,2);
    require(engine->control(sub->task,StopReason::Pause)==FETCHKIT_OK && done(sub).state==FETCHKIT_PAUSED,"queued first pause");
    require(engine->control(sub->task,StopReason::None,true)==FETCHKIT_OK,"queued resume");
    require(engine->control(sub->task,StopReason::Pause)==FETCHKIT_OK && done(sub).generation==2,"queued second pause");
    const auto id=std::string(read(sub).persistent_id);engine->close();
    engine=std::make_shared<Engine>(cfg,transport);require(engine->storage()->find(id)->generation==2,"queued generation lost on close");engine->close();
}
void clockJump(const std::string& root) {
    struct SplitClock final : Clock {
        std::atomic<std::uint64_t> wall{1700000000000ULL},steady{1000};
        std::uint64_t nowMs() const override{return wall.load();}
        std::uint64_t steadyMs() const override{return steady.load();}
    };
    auto clock=std::make_shared<SplitClock>();auto cfg=configFor(root);cfg.clock=clock;cfg.max_retries=1;cfg.retry_base_ms=10;
    auto transport=std::make_shared<FakeTransport>();auto r=request("clock");transport->set(r.request.url,{1024,false,false,1,503,"120"});
    auto engine=std::make_shared<Engine>(cfg,transport);auto sub=engine->enqueue(r,1);
    eventually([&]{return read(sub).state==FETCHKIT_RETRY_WAITING;},"clock wait");clock->wall+=3600000;engine->wake();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));require(transport->count(r.request.url)==1,"wall clock jump bypassed wait");
    clock->steady+=120000;engine->wake();completed(sub);engine->close();
}
void closedStorage(const std::string& root) {
    auto transport=std::make_shared<FakeTransport>();auto r=request("closed");transport->set(r.request.url,{8192});
    auto engine=std::make_shared<Engine>(configFor(root),transport);auto sub=engine->enqueue(r,1);completed(sub);
    auto store=engine->storage();const auto id=std::string(read(sub).persistent_id);auto lease=store->acquire(id);
    engine->close();bool closed=false;try{store->pin(id,true);}catch(const Failure& e){closed=e.code==FETCHKIT_CLOSED;}
    require(closed,"captured storage mutated after close");std::array<std::uint8_t,8> bytes{};
    require(lease->read(0,bytes.data(),bytes.size())==8,"close invalidated lease");
}
}
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    const auto run=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::pair<const char*,void(*)(const std::string&)> cases[]={
        {"parallelism-and-control",concurrency},{"shared-subscriptions",sharing},{"provider-lifetime",providerLifetime},
        {"event-backpressure",events},{"slow-callback-close",slowCallback},{"persistent-retry",retryPersistence},
        {"retry-budget",retryBudget},{"quota-reservations",quota},{"unknown-length-quota",unknownQuota},
        {"auth-request-recovery",authRecovery},{"explicit-request-source",requestSourceRetry},
        {"request-retry-budget",requestRetryBudget},{"request-source-release-barrier",requestRetryHandoff},{"request-policy-c-abi",requestPolicyCAbi},{"direct-task-query",directTaskQuery},
        {"request-retry-admission",requestRetryAdmission},{"request-retry-close-race",requestRetryCloseRace},
        {"lru-pin-expiry-lease",cache},{"staging-recount-retention",retention},{"retry-date-policy",dates},
        {"expired-pinned-refresh",expiredRefresh},{"retained-subscription-bound",subscriberLimit},
        {"callback-reentrancy",reentrancy},{"64-bit-ledger",largeLedger},{"cancel-pause-release-close-races",closeRaces},
        {"stale-subscription-isolation",staleSubscription},{"queued-generation-checkpoint",queuedGeneration},
        {"monotonic-retry-clock",clockJump},{"closed-storage-write-barrier",closedStorage}};
    for(const auto& item:cases) {
        try {item.second(std::string(argv[1])+"/"+run+"/"+item.first);std::cout<<item.first<<": passed\n";}
        catch(const std::exception& error){std::cerr<<item.first<<": "<<error.what()<<'\n';return 1;}
    }
    std::cout<<"29 scheduler/storage suites passed\n";return 0;
}
