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
    explicit ResourceStore(const std::string& root);
    std::filesystem::path stagingPath(const std::string& id) const;
    std::filesystem::path objectPath(const std::string& id) const;
    std::optional<Record> find(const std::string& id);
    Record create(const core::DownloadRequest& request);
    void save(const Record& record);
    std::vector<Record> list(std::uint32_t offset, std::uint32_t capacity);
    fetchkit_storage_stats stats();
    void discard(Record& record);
    void publish(Record& record, transport::Cancellation& cancellation,
        const std::function<void()>& publication_barrier);
    void checkObject(const Record& record);
private:
    void scan();
    void repair(Record& record);
    void quarantine(const std::filesystem::path& path);
    StagingStore directory_;
    std::mutex mutex_;
    std::map<std::string, Record> records_;
    std::set<std::string> blocked_ids_;
    std::uint32_t quarantined_ = 0, unsupported_ = 0, orphans_ = 0;
};

std::string sha256File(StagingFile& file, transport::Cancellation* cancellation = nullptr);
std::uint32_t verifyFile(const std::filesystem::path& path, const Record& record,
    transport::Cancellation* cancellation = nullptr);

class ResourceLease {
public:
    ResourceLease(std::shared_ptr<ResourceStore> store, const Record& record);
    std::uint32_t read(std::uint64_t offset, std::uint8_t* buffer, std::uint32_t capacity);
private:
    std::shared_ptr<ResourceStore> store_; // Lock outlives engine; declared before file.
    StagingFile file_;
    std::mutex mutex_;
};
}
