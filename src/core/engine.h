#pragma once

#include "core/download_types.h"
#include "storage/resource_store.h"

#include <deque>
#include <random>
#include <thread>
#include <unordered_map>

namespace fetchkit::core {

class Engine;
struct Subscription;
enum class StopReason { None, Pause, Cancel };

struct ResumeData {
    std::uint64_t offset = 0;
    std::optional<std::uint64_t> total;
    std::string strong_etag;
};

struct Task : public std::enable_shared_from_this<Task> {
    DownloadRequest request;
    std::filesystem::path path;
    std::weak_ptr<Engine> owner;
    std::atomic<Engine*> event_owner{nullptr}; // Engine joins workers before clearing/destroying this target.
    std::mutex mutex;
    std::condition_variable changed;
    fetchkit_task_snapshot snapshot{};
    std::shared_ptr<transport::Cancellation> cancellation;
    std::atomic<StopReason> stop{StopReason::None};
    ResumeData saved; // Worker-owned; never read by public API threads.
    storage::Record record;
    std::weak_ptr<storage::ResourceStore> store;
    std::vector<std::weak_ptr<Subscription>> subscribers; // Engine mutex.
    bool finishing = false, processing = false, queued = false, refresh = false;
    std::uint64_t retry_after_ms = 0;
    std::uint64_t retry_monotonic_at = 0;
    std::chrono::steady_clock::time_point last_progress{};
    void progress(std::uint64_t offset, std::optional<std::uint64_t> total);
};

struct Subscription : public std::enable_shared_from_this<Subscription> {
    std::shared_ptr<Task> task;
    DownloadRequest request;
    std::uint64_t id = 0;
    std::atomic<bool> inactive{false}, released{false}, suppressed{false};
    std::mutex mutex;
    std::condition_variable quiescent;
    std::deque<fetchkit_event> events;
    std::shared_ptr<transport::Cancellation> provider_cancellation;
    std::uint32_t provider_calls = 0;
    fetchkit_status error = FETCHKIT_OK;
    bool dispatching = false;
    std::thread::id dispatch_thread;
    fetchkit_task_snapshot read(std::uint32_t timeout_ms, bool& ready);
    fetchkit_status dispatch(fetchkit_event_callback callback, void* user, std::uint32_t maximum, std::uint32_t& count);
};

struct RunResult { fetchkit_status error = FETCHKIT_OK; int platform_code = 0; };
RunResult runDownload(Task& task, const EngineConfig& config, transport::HttpTransport& transport);

class Engine : public std::enable_shared_from_this<Engine> {
public:
    Engine(EngineConfig config, std::shared_ptr<transport::HttpTransport> transport);
    ~Engine();
    std::shared_ptr<Subscription> enqueue(DownloadRequest request, std::uint64_t id,
        const std::function<void(const std::shared_ptr<Subscription>&)>& publish = {}, bool start = true);
    fetchkit_status control(const std::shared_ptr<Task>& task, StopReason reason, bool resume = false);
    fetchkit_status cancel(const std::shared_ptr<Subscription>& sub, fetchkit_status error = FETCHKIT_CANCELLED);
    fetchkit_status release(const std::shared_ptr<Subscription>& sub);
    fetchkit_status close();
    bool onWorker() const;
    std::shared_ptr<storage::ResourceStore> storage();
    void notifyProgress(const std::shared_ptr<Task>& task);
    void decorate(const std::shared_ptr<Subscription>& sub, fetchkit_task_snapshot& snapshot);
    std::optional<fetchkit_task_snapshot> liveSnapshot(const std::string& id);
    void wake() { changed_.notify_all(); }
private:
    void loop(bool controls_only);
    void run(const std::shared_ptr<Task>& task);
    void queueLocked(const std::shared_ptr<Task>& task);
    void emitLocked(const std::shared_ptr<Task>& task, std::uint32_t kind = FETCHKIT_EVENT_STATE);
    std::size_t liveCount(const std::shared_ptr<Task>& task) const;
    void requestStopLocked(const std::shared_ptr<Task>& task, StopReason reason);
    void provide(const std::shared_ptr<Task>& task, const ProviderContext& context, RequestData& data,
        transport::Cancellation& cancellation);
    EngineConfig config_;
    std::shared_ptr<transport::HttpTransport> transport_;
    std::shared_ptr<storage::ResourceStore> store_;
    std::mutex mutex_, close_mutex_, admission_mutex_;
    std::condition_variable changed_;
    std::deque<std::shared_ptr<Task>> pending_;
    std::unordered_map<std::string, std::shared_ptr<Task>> identities_;
    std::unordered_map<std::uint64_t, std::weak_ptr<Subscription>> subscriptions_;
    std::vector<std::thread> workers_;
    std::mt19937_64 random_;
    bool closing_ = false;
};

}
