#pragma once
#include "storage/record.h"
#include "storage/staging_file.h"
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace fetchkit::storage {
class ResourceStore : public std::enable_shared_from_this<ResourceStore> {
public:
    explicit ResourceStore(const std::string& root, std::uint64_t capacity = INT64_MAX,
        std::uint64_t retention_ms = 604800000, std::shared_ptr<core::Clock> clock = std::make_shared<core::SystemClock>());
    std::filesystem::path stagingPath(const std::string& id) const;
    std::filesystem::path objectPath(const std::string& id) const;
    std::optional<Record> find(const std::string& id);
    Record create(const core::DownloadRequest& request);
    void save(Record record, bool policy_update = false);
    std::vector<Record> list(std::uint32_t offset, std::uint32_t capacity);
    fetchkit_storage_stats stats();
    void discard(Record& record);
    void publish(Record& record, transport::Cancellation& cancellation,
        const std::function<void()>& publication_barrier);
    void checkObject(const Record& record);
    void protect(const std::string& id, bool require_unleased = false, bool require_existing = false);
    void unprotect(const std::string& id);
    void reserve(const std::string& id, std::uint64_t next_offset, std::optional<std::uint64_t> total);
    void observeSize(const std::string& id, std::uint64_t size);
    void endAttempt(const std::string& id);
    Record recreate(const Record& record);
    std::shared_ptr<class ResourceLease> acquire(const std::string& id);
    void releaseLease(const std::string& id) noexcept;
    void pin(const std::string& id, bool pinned);
    void setExpiry(const std::string& id, std::uint64_t expires_at_ms);
    void remove(const std::string& id);
    void trimTo(std::uint64_t target);
    void collectStaging();
    bool available(const std::string& id);
    void shutdown(); // Stop mutation; existing lease reads retain their context.
    static constexpr std::uint64_t metadataBudget() { return 2ULL * 65556; }
private:
    void scan();
    void repair(Record& record);
    void quarantine(const std::filesystem::path& path);
    StagingStore directory_;
    void recount();
    std::uint64_t charged() const;
    bool evictOne(const std::string& exclude = {});
    void erase(const std::string& id, bool own_task = false);
    void ensureCapacity(const std::string& exclude);
    bool protectedId(const std::string& id) const;
    std::recursive_mutex mutex_;
    mutable std::mutex protection_mutex_;
    std::map<std::string, Record> records_;
    std::set<std::string> blocked_ids_;
    std::set<std::string> protected_ids_, deleting_ids_, present_ids_, slots_;
    std::map<std::string, std::uint32_t> leases_;
    std::uint32_t lease_count_ = 0;
    std::map<std::string, std::uint64_t> data_bytes_, allocations_, record_bytes_;
    std::uint64_t capacity_, retention_ms_, unowned_bytes_ = 0, access_order_ = 0;
    std::shared_ptr<core::Clock> clock_;
    bool scanning_ = true;
    bool closed_ = false;
    void requireOpen() const;
    std::uint32_t quarantined_ = 0, unsupported_ = 0, orphans_ = 0;
};

std::string sha256File(StagingFile& file, transport::Cancellation* cancellation = nullptr);
std::uint32_t verifyFile(const std::filesystem::path& path, const Record& record,
    transport::Cancellation* cancellation = nullptr);

class ResourceLease {
public:
    ResourceLease(std::shared_ptr<ResourceStore> store, const Record& record);
    ~ResourceLease();
    std::uint32_t read(std::uint64_t offset, std::uint8_t* buffer, std::uint32_t capacity);
private:
    std::shared_ptr<ResourceStore> store_; // Lock outlives engine; declared before file.
    std::string id_;
    std::unique_ptr<StagingFile> file_;
    std::mutex mutex_;
};
}
