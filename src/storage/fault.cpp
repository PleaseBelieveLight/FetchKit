#include "storage/fault.h"
#include "core/download_types.h"
#include <algorithm>

#ifdef FETCHKIT_ENABLE_FAULT_INJECTION
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unordered_map>
#endif

namespace fetchkit::storage {
void faultPoint(const char* name, transport::Cancellation* cancellation) {
#ifdef FETCHKIT_ENABLE_FAULT_INJECTION
    const auto point = std::getenv("FETCHKIT_FAULT_POINT");
    if (!point || std::strcmp(point, name)) return;
    static std::mutex mutex;
    static std::unordered_map<std::string, unsigned long> hits;
    const auto occurrence = std::getenv("FETCHKIT_FAULT_OCCURRENCE");
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (++hits[name] != (occurrence ? std::strtoul(occurrence, nullptr, 10) : 1UL)) return;
    }
    const auto action = std::getenv("FETCHKIT_FAULT_ACTION");
    if (action && std::strcmp(action, "disk_full") == 0) throw core::Failure(FETCHKIT_DISK_FULL);
    if (action && std::strcmp(action, "permission") == 0) throw core::Failure(FETCHKIT_PERMISSION_DENIED);
    if (action && std::strcmp(action, "io") == 0) throw core::Failure(FETCHKIT_IO_ERROR);
    if(action && std::strcmp(action,"wait_cancel")==0 && cancellation) {
        while(!cancellation->isCancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return;
    }
    std::fprintf(stdout, "{\"fault_barrier\":\"%s\"}\n", name);
    std::fflush(stdout);
    for (;;) std::this_thread::sleep_for(std::chrono::milliseconds(20));
#else
    (void)name;
    (void)cancellation;
#endif
}

std::size_t writeLimit(std::size_t requested) {
#ifdef FETCHKIT_ENABLE_FAULT_INJECTION
    if (std::getenv("FETCHKIT_TEST_SHORT_WRITES")) return std::min<std::size_t>(requested, 7);
#endif
    return requested;
}
}
