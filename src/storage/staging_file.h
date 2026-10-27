#pragma once

#include "core/download_types.h"

#include <filesystem>

namespace fetchkit::storage {

class StagingFile {
public:
    StagingFile(const std::filesystem::path& path, bool create, bool read_only = false);
    ~StagingFile();
    StagingFile(const StagingFile&) = delete;
    StagingFile& operator=(const StagingFile&) = delete;
    std::uint64_t size() const;
    void seek(std::uint64_t offset);
    void truncate(std::uint64_t length = 0);
    std::size_t read(std::uint8_t* data, std::size_t size);
    void write(const std::uint8_t* data, std::size_t size);
    void sync();
private:
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int handle_ = -1;
#endif
};

class StagingStore {
public:
    explicit StagingStore(const std::string& root);
    ~StagingStore();
    StagingStore(const StagingStore&) = delete;
    StagingStore& operator=(const StagingStore&) = delete;
    std::filesystem::path reserve();
    const std::filesystem::path& root() const { return root_; }
private:
    std::filesystem::path root_;
    std::uint64_t next_id_ = 1;
#ifdef _WIN32
    void* lock_ = nullptr;
#else
    int lock_ = -1;
#endif
};

void syncDirectory(const std::filesystem::path& path);
void atomicRename(const std::filesystem::path& from, const std::filesystem::path& to, bool replace);
void removeFile(const std::filesystem::path& path);
bool regularFile(const std::filesystem::path& path);

}
