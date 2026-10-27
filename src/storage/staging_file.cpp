#include "storage/staging_file.h"
#include "storage/fault.h"

#include <cerrno>
#include <algorithm>
#include <limits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fetchkit::storage {
namespace {

[[noreturn]] void failIo(int code) {
#ifdef _WIN32
    const auto status = code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL ? FETCHKIT_DISK_FULL :
        code == ERROR_ACCESS_DENIED ? FETCHKIT_PERMISSION_DENIED : FETCHKIT_IO_ERROR;
#else
    const auto status = code == ENOSPC || code == EDQUOT ? FETCHKIT_DISK_FULL :
        code == EACCES || code == EPERM || code == ELOOP ? FETCHKIT_PERMISSION_DENIED : FETCHKIT_IO_ERROR;
#endif
    throw core::Failure(status, code);
}

}

StagingFile::StagingFile(const std::filesystem::path& path, bool create, bool read_only) {
#ifdef _WIN32
    handle_ = CreateFileW(path.c_str(), read_only ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE,
        read_only ? FILE_SHARE_READ : 0, nullptr,
        create ? CREATE_NEW : OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        handle_ = nullptr;
        if (create && GetLastError() == ERROR_FILE_EXISTS) throw core::Failure(FETCHKIT_BUSY);
        failIo(static_cast<int>(GetLastError()));
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle_, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        CloseHandle(handle_); handle_ = nullptr;
        throw core::Failure(FETCHKIT_PERMISSION_DENIED);
    }
#else
    static_assert(sizeof(off_t) >= 8, "64-bit file offsets required");
    handle_ = ::open(path.c_str(), (read_only ? O_RDONLY : O_RDWR) | O_CLOEXEC | O_NOFOLLOW | (create ? O_CREAT | O_EXCL : 0), 0600);
    if (handle_ < 0) {
        if (create && errno == EEXIST) throw core::Failure(FETCHKIT_BUSY);
        failIo(errno);
    }
    struct stat info{};
    if (::fstat(handle_, &info) != 0 || !S_ISREG(info.st_mode)) {
        ::close(handle_); handle_ = -1;
        throw core::Failure(FETCHKIT_PERMISSION_DENIED);
    }
#endif
}

StagingFile::~StagingFile() {
#ifdef _WIN32
    if (handle_) CloseHandle(handle_);
#else
    if (handle_ >= 0) ::close(handle_);
#endif
}

std::uint64_t StagingFile::size() const {
#ifdef _WIN32
    LARGE_INTEGER value{};
    if (!GetFileSizeEx(handle_, &value)) failIo(static_cast<int>(GetLastError()));
    return static_cast<std::uint64_t>(value.QuadPart);
#else
    struct stat value{};
    if (::fstat(handle_, &value) != 0) failIo(errno);
    return static_cast<std::uint64_t>(value.st_size);
#endif
}

void StagingFile::seek(std::uint64_t offset) {
    if (offset > static_cast<std::uint64_t>(INT64_MAX)) throw core::Failure(FETCHKIT_PROTOCOL_MISMATCH);
#ifdef _WIN32
    LARGE_INTEGER value{}; value.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle_, value, nullptr, FILE_BEGIN)) failIo(static_cast<int>(GetLastError()));
#else
    if (::lseek(handle_, static_cast<off_t>(offset), SEEK_SET) < 0) failIo(errno);
#endif
}

void StagingFile::truncate(std::uint64_t length) {
    seek(length);
#ifdef _WIN32
    if (!SetEndOfFile(handle_)) failIo(static_cast<int>(GetLastError()));
#else
    if (::ftruncate(handle_, static_cast<off_t>(length)) != 0) failIo(errno);
#endif
}

void StagingFile::write(const std::uint8_t* data, std::size_t count) {
    while (count) {
#ifdef _WIN32
        DWORD written = 0;
        const auto block = static_cast<DWORD>(writeLimit(std::min<std::size_t>(count, 65536)));
        if (!WriteFile(handle_, data, block, &written, nullptr)) failIo(static_cast<int>(GetLastError()));
#else
        const auto written = ::write(handle_, data, writeLimit(count));
        if (written < 0) { if (errno == EINTR) continue; failIo(errno); }
#endif
        if (written == 0) throw core::Failure(FETCHKIT_IO_ERROR);
        data += written;
        count -= static_cast<std::size_t>(written);
    }
}

std::size_t StagingFile::read(std::uint8_t* data, std::size_t count) {
#ifdef _WIN32
    DWORD received = 0;
    if (!ReadFile(handle_, data, static_cast<DWORD>(std::min<std::size_t>(count, 65536)), &received, nullptr)) failIo(static_cast<int>(GetLastError()));
    return received;
#else
    for (;;) {
        const auto received = ::read(handle_, data, count);
        if (received >= 0) return static_cast<std::size_t>(received);
        if (errno != EINTR) failIo(errno);
    }
#endif
}

void StagingFile::sync() {
#ifdef _WIN32
    if (!FlushFileBuffers(handle_)) failIo(static_cast<int>(GetLastError()));
#else
    while (::fsync(handle_) != 0) { if (errno == EINTR) continue; failIo(errno); }
#endif
}

StagingStore::StagingStore(const std::string& root) : root_(std::filesystem::absolute(std::filesystem::u8path(root))) {
    if (root_.u8string().size() > 3500) throw core::Failure(FETCHKIT_INVALID_ARGUMENT);
    std::error_code error;
    std::filesystem::create_directories(root_, error);
    if (error) throw core::Failure(FETCHKIT_PERMISSION_DENIED, error.value());
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(root_))) throw core::Failure(FETCHKIT_PERMISSION_DENIED);
    const auto lock_path = root_ / "fetchkit.lock";
#ifdef _WIN32
    lock_ = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (lock_ == INVALID_HANDLE_VALUE) {
        lock_ = nullptr;
        if (GetLastError() == ERROR_SHARING_VIOLATION) throw core::Failure(FETCHKIT_BUSY);
        failIo(static_cast<int>(GetLastError()));
    }
#else
    lock_ = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_ < 0) failIo(errno);
    if (::flock(lock_, LOCK_EX | LOCK_NB) != 0) {
        const int code = errno;
        ::close(lock_); lock_ = -1;
        if (code == EWOULDBLOCK || code == EAGAIN) throw core::Failure(FETCHKIT_BUSY);
        failIo(code);
    }
#endif
    // Install ownership before any repair. Every SDK subdirectory must be real.
    try {
        for (const auto name : {"tmp", "records", "objects", "quarantine"}) {
            const auto path = root_ / name;
            std::filesystem::create_directory(path, error);
            if (error || std::filesystem::is_symlink(std::filesystem::symlink_status(path)) || !std::filesystem::is_directory(path)) {
                throw core::Failure(FETCHKIT_PERMISSION_DENIED, error.value());
            }
        }
        syncDirectory(root_);
    } catch (...) {
#ifdef _WIN32
        CloseHandle(lock_); lock_ = nullptr;
#else
        ::close(lock_); lock_ = -1;
#endif
        throw;
    }
}

StagingStore::~StagingStore() {
#ifdef _WIN32
    if (lock_) CloseHandle(lock_);
#else
    if (lock_ >= 0) ::close(lock_);
#endif
}

std::filesystem::path StagingStore::reserve() {
    for (unsigned int attempt = 0; attempt < 100000; ++attempt) {
        const auto path = root_ / "tmp" / (std::to_string(next_id_++) + ".part");
        try { StagingFile file(path, true); return path; }
        catch (const core::Failure& error) { if (error.code != FETCHKIT_BUSY) throw; }
    }
    throw core::Failure(FETCHKIT_BUSY);
}

bool regularFile(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) return false;
    if (error) throw core::Failure(FETCHKIT_IO_ERROR, error.value());
    if (std::filesystem::is_symlink(status)) throw core::Failure(FETCHKIT_PERMISSION_DENIED);
    if (!std::filesystem::exists(status)) return false;
    if (!std::filesystem::is_regular_file(status)) throw core::Failure(FETCHKIT_PERMISSION_DENIED);
    return true;
}

void syncDirectory(const std::filesystem::path& path) {
#ifndef _WIN32
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) failIo(errno);
    int result;
    do { result = ::fsync(fd); } while (result != 0 && errno == EINTR);
    const int error = errno;
    ::close(fd);
    if (result != 0) failIo(error);
#else
    // Win32 has no portable directory fsync. MOVEFILE_WRITE_THROUGH is used;
    // only process termination recovery is claimed until reboot tests exist.
    (void)path;
#endif
}

void atomicRename(const std::filesystem::path& from, const std::filesystem::path& to, bool replace) {
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0))) failIo(static_cast<int>(GetLastError()));
#else
    if (!replace && std::filesystem::exists(std::filesystem::symlink_status(to))) throw core::Failure(FETCHKIT_BUSY);
    if (::rename(from.c_str(), to.c_str()) != 0) failIo(errno);
#endif
}

void removeFile(const std::filesystem::path& path) {
    if (!regularFile(path)) return;
    std::error_code error;
    std::filesystem::remove(path, error);
    if (error) throw core::Failure(FETCHKIT_IO_ERROR, error.value());
    syncDirectory(path.parent_path());
}

}
