#pragma once

#include "fetchkit/fetchkit.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace fetchkit {

inline fetchkit_string stringView(const std::string& value) noexcept {
    return {value.data(), static_cast<std::uint64_t>(value.size())};
}

inline void checkStatus(fetchkit_status status) {
    if (status != FETCHKIT_OK) throw std::runtime_error(fetchkit_status_name(status));
}

class Task {
public:
    explicit Task(fetchkit_task handle = 0) noexcept : handle_(handle) {}
    ~Task() { if (handle_) fetchkit_task_release(handle_); }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, 0)) {}
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) fetchkit_task_release(handle_);
            handle_ = std::exchange(other.handle_, 0);
        }
        return *this;
    }
    fetchkit_task_snapshot query() const {
        fetchkit_task_snapshot snapshot{};
        snapshot.struct_size = sizeof(snapshot);
        checkStatus(fetchkit_task_query(handle_, &snapshot));
        return snapshot;
    }
    fetchkit_task_snapshot wait(std::uint32_t timeout_ms = 1000) const {
        fetchkit_task_snapshot snapshot{};
        snapshot.struct_size = sizeof(snapshot);
        const auto status = fetchkit_task_wait(handle_, timeout_ms, &snapshot);
        if (status != FETCHKIT_TIMEOUT) checkStatus(status);
        return snapshot;
    }
    fetchkit_status pause() { return fetchkit_task_pause(handle_); }
    fetchkit_status resume() { return fetchkit_task_resume(handle_); }
    fetchkit_status cancel() { return fetchkit_task_cancel(handle_); }
    fetchkit_task nativeHandle() const noexcept { return handle_; }
private:
    fetchkit_task handle_ = 0;
};

class Engine {
public:
    explicit Engine(const fetchkit_engine_config& config) { checkStatus(fetchkit_engine_create(&config, &handle_)); }
    ~Engine() { if (handle_) fetchkit_engine_close(handle_); }
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&& other) noexcept : handle_(std::exchange(other.handle_, 0)) {}
    Engine& operator=(Engine&& other) noexcept {
        if (this != &other) {
            if (handle_) fetchkit_engine_close(handle_);
            handle_ = std::exchange(other.handle_, 0);
        }
        return *this;
    }
    Task enqueue(const fetchkit_download_request& request) {
        fetchkit_task task = 0;
        checkStatus(fetchkit_enqueue(handle_, &request, &task));
        return Task(task);
    }
private:
    fetchkit_engine handle_ = 0;
};

}
