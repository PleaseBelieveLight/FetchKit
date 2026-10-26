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
      store_(std::make_unique<storage::StagingStore>(config_.root)) {
    worker_ = std::thread([this] { loop(); });
    worker_id_ = worker_.get_id();
}

Engine::~Engine() { close(); }

std::shared_ptr<Task> Engine::enqueue(DownloadRequest request, std::uint64_t id,
    const std::function<void(const std::shared_ptr<Task>&)>& publish) {
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
    if (active_ || !identities_[identity].expired()) throw Failure(FETCHKIT_BUSY);
    task->path = store_->reserve();
    task->owner = shared_from_this();
    const auto path = task->path.u8string();
    std::memcpy(task->snapshot.staging_path, path.c_str(), path.size() + 1);
    identities_[identity] = task;
    if (publish) publish(task);
    active_ = pending_ = task;
    changed_.notify_one();
    return task;
}

fetchkit_status Engine::control(const std::shared_ptr<Task>& task, StopReason reason, bool resume) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_) return FETCHKIT_CLOSED;
    std::lock_guard<std::mutex> task_lock(task->mutex);
    auto& snapshot = task->snapshot;
    if (task->finishing) return FETCHKIT_BUSY;
    if (resume) {
        if (task->released) return FETCHKIT_CLOSED;
        if (active_) return FETCHKIT_BUSY;
        if (snapshot.state != FETCHKIT_PAUSED && !(snapshot.state == FETCHKIT_FAILED && snapshot.can_resume)) return FETCHKIT_BUSY;
        task->cancellation = std::make_shared<transport::Cancellation>();
        task->stop.store(StopReason::None);
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
    if (active_ != task || (snapshot.state != FETCHKIT_RUNNING && snapshot.state != FETCHKIT_QUEUED && snapshot.state != FETCHKIT_PAUSING)) {
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
    store_.reset();
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
        StopReason stop;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::lock_guard<std::mutex> task_lock(task->mutex);
            task->finishing = true;
            stop = task->stop.load();
        }
        std::uint32_t state = FETCHKIT_FAILED;
        if (stop == StopReason::Cancel) {
            std::error_code error;
            std::filesystem::remove(task->path, error);
            result = {error ? FETCHKIT_IO_ERROR : FETCHKIT_CANCELLED, error.value()};
            state = error ? FETCHKIT_FAILED : FETCHKIT_TASK_CANCELLED;
        } else if (stop == StopReason::Pause && (result.error == FETCHKIT_CANCELLED || result.error == FETCHKIT_OK)) {
            state = FETCHKIT_PAUSED;
            result = {};
        } else if (result.error == FETCHKIT_OK) {
            state = FETCHKIT_TRANSFER_COMPLETE;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::lock_guard<std::mutex> task_lock(task->mutex);
            task->snapshot.state = state;
            task->snapshot.error = result.error;
            task->snapshot.platform_code = result.platform_code;
            task->snapshot.can_resume = state == FETCHKIT_PAUSED ||
                (state == FETCHKIT_FAILED && (result.error == FETCHKIT_NETWORK || result.error == FETCHKIT_TIMEOUT));
            active_.reset();
            task->finishing = false;
            task->changed.notify_all();
        }
    }
}

}
