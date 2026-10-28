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

enum class LogLevel : std::uint32_t {
    Off = FETCHKIT_LOG_OFF, Error = FETCHKIT_LOG_ERROR, Warn = FETCHKIT_LOG_WARN, Debug = FETCHKIT_LOG_DEBUG
};

// 当前库实例内的进程级开关，默认 Off；主机诊断写 stderr，不改变错误或事件语义。
inline void setLogLevel(LogLevel level) { checkStatus(fetchkit_set_log_level(static_cast<std::uint32_t>(level))); }

// 独立订阅的 RAII 包装；析构等同于 release，最后一个释放保留可恢复记录。
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
    std::uint32_t dispatch(fetchkit_event_callback callback,void* user,std::uint32_t maximum=64) {
        std::uint32_t count=0;checkStatus(fetchkit_task_dispatch(handle_,callback,user,maximum,&count));return count;
    }
    fetchkit_task nativeHandle() const noexcept { return handle_; }
private:
    fetchkit_task handle_ = 0;
};

// 已发布资源的只读 RAII 租约；engine 关闭后仍可读，析构后才能解除其淘汰/目录锁保护。
class ResourceLease {
public:
    explicit ResourceLease(fetchkit_lease handle=0) noexcept : handle_(handle) {}
    ~ResourceLease() { if(handle_) fetchkit_resource_release(handle_); }
    ResourceLease(const ResourceLease&)=delete;
    ResourceLease& operator=(const ResourceLease&)=delete;
    ResourceLease(ResourceLease&& other) noexcept : handle_(std::exchange(other.handle_,0)) {}
    ResourceLease& operator=(ResourceLease&& other) noexcept {
        if(this!=&other) { if(handle_) fetchkit_resource_release(handle_);handle_=std::exchange(other.handle_,0); }
        return *this;
    }
    std::uint32_t read(std::uint64_t offset,void* buffer,std::uint32_t capacity) const {
        std::uint32_t count=0;checkStatus(fetchkit_resource_read(handle_,offset,buffer,capacity,&count));return count;
    }
private:
    fetchkit_lease handle_=0;
};

// 同步主机 API；构造/析构可能执行磁盘工作或等待 I/O/provider，宿主须选择合适线程。
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
    Task attach(const fetchkit_download_request& request) {
        fetchkit_task task=0;checkStatus(fetchkit_attach(handle_,&request,&task));return Task(task);
    }
    ResourceLease acquire(const std::string& persistent_id) {
        fetchkit_lease lease=0;checkStatus(fetchkit_resource_acquire(handle_,stringView(persistent_id),&lease));return ResourceLease(lease);
    }
    fetchkit_engine nativeHandle() const noexcept { return handle_; }
    fetchkit_status close() {
        if(!handle_) return FETCHKIT_CLOSED;
        const auto status=fetchkit_engine_close(handle_);if(status==FETCHKIT_OK) handle_=0;return status;
    }
    void pin(const std::string& id,bool value=true) {checkStatus(fetchkit_resource_pin(handle_,stringView(id),value));}
    void setExpiry(const std::string& id,std::uint64_t expires_at_ms) {checkStatus(fetchkit_resource_set_expiry(handle_,stringView(id),expires_at_ms));}
    void remove(const std::string& id) {checkStatus(fetchkit_resource_remove(handle_,stringView(id)));}
    void trimTo(std::uint64_t bytes) {checkStatus(fetchkit_storage_trim(handle_,bytes));}
    void collectStaging() {checkStatus(fetchkit_storage_collect_staging(handle_));}
private:
    fetchkit_engine handle_ = 0;
};

}
