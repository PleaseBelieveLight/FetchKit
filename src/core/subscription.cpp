#include "core/engine.h"

namespace fetchkit::core {
fetchkit_task_snapshot Subscription::read(std::uint32_t timeout, bool& ready) {
    const auto owner=task->owner.lock();
    if(timeout && owner && owner->onWorker()) throw Failure(FETCHKIT_BUSY);
    fetchkit_task_snapshot result;
    {
        std::unique_lock<std::mutex> lock(task->mutex);
        ready=!timeout || task->changed.wait_for(lock,std::chrono::milliseconds(timeout),[&] {
            return inactive.load() || stoppedState(task->snapshot.state);
        });
        result=task->snapshot;
    }
    result.task_id=id;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if(inactive) {
            result.state=error==FETCHKIT_CANCELLED ? FETCHKIT_TASK_CANCELLED : FETCHKIT_FAILED;
            result.error=error;result.can_resume=0;
        }
    }
    if(owner) owner->decorate(shared_from_this(),result);
    return result;
}

fetchkit_status Subscription::dispatch(fetchkit_event_callback callback,void* user,std::uint32_t maximum,std::uint32_t& count) {
    count=0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if(released || suppressed) return FETCHKIT_CLOSED;
        if(dispatching) return FETCHKIT_BUSY;
        dispatching=true;dispatch_thread=std::this_thread::get_id();
    }
    fetchkit_status status=FETCHKIT_OK;
    while(count<maximum) {
        fetchkit_event event{};
        {
            std::lock_guard<std::mutex> lock(mutex);
            if(released || suppressed || events.empty()) break;
            event=events.front();events.pop_front();
        }
        try { callback(user,&event);++count; }
        catch(...) { status=FETCHKIT_INTERNAL_ERROR;break; }
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        dispatching=false;dispatch_thread={};quiescent.notify_all();
    }
    if(status!=FETCHKIT_OK) if(const auto owner=task->owner.lock()) owner->cancel(shared_from_this(),status);
    return status;
}
}
