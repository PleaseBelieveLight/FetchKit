#pragma once

#include "core/download_types.h"
#include "storage/resource_store.h"

#include <deque>
#include <memory>
#include <thread>
#include <unordered_map>

namespace fetchkit::core {

class Engine;
enum class StopReason { None, Pause, Cancel };

struct ResumeData {
    std::uint64_t offset = 0;
    std::optional<std::uint64_t> total;
    std::string strong_etag;
};

struct Task {
    DownloadRequest request;
    std::filesystem::path path;
    std::weak_ptr<Engine> owner;
    std::mutex mutex;
    std::condition_variable changed;
    fetchkit_task_snapshot snapshot{};
    std::shared_ptr<transport::Cancellation> cancellation;
    std::atomic<StopReason> stop{StopReason::None};
    ResumeData saved; // Worker-owned; never read by public API threads.
    storage::Record record;
    std::weak_ptr<storage::ResourceStore> store;
    bool finishing = false, released = false;
    void progress(std::uint64_t offset, std::optional<std::uint64_t> total);
};

struct RunResult { fetchkit_status error = FETCHKIT_OK; int platform_code = 0; };
RunResult runDownload(Task& task, const EngineConfig& config, transport::HttpTransport& transport);

class Engine : public std::enable_shared_from_this<Engine> {
public:
    Engine(EngineConfig config, std::shared_ptr<transport::HttpTransport> transport);
    ~Engine();
    std::shared_ptr<Task> enqueue(DownloadRequest request, std::uint64_t id,
        const std::function<void(const std::shared_ptr<Task>&)>& publish = {}, bool start = true);
    fetchkit_status control(const std::shared_ptr<Task>& task, StopReason reason, bool resume = false);
    fetchkit_status close();
    bool onWorker() const { return std::this_thread::get_id() == worker_id_; }
    std::shared_ptr<storage::ResourceStore> storage();
private:
    void loop();
    EngineConfig config_;
    std::shared_ptr<transport::HttpTransport> transport_;
    std::shared_ptr<storage::ResourceStore> store_;
    std::mutex mutex_, close_mutex_;
    std::condition_variable changed_;
    std::shared_ptr<Task> active_, pending_;
    std::unordered_map<std::string, std::weak_ptr<Task>> identities_;
    bool closing_ = false;
    std::thread worker_;
    std::thread::id worker_id_;
};

}
