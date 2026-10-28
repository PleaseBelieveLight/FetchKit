#include "core/diagnostics.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace {
std::atomic<std::uint32_t> log_level{FETCHKIT_LOG_OFF};

void writeLog(std::uint32_t level, const char* message) noexcept {
    if (!fetchkit::core::diagnosticEnabled(level)) return;
#ifdef __ANDROID__
    const int priority = level == FETCHKIT_LOG_ERROR ? ANDROID_LOG_ERROR :
        level == FETCHKIT_LOG_WARN ? ANDROID_LOG_WARN : ANDROID_LOG_DEBUG;
    __android_log_write(priority, "FetchKit", message);
#else
    const char* name = level == FETCHKIT_LOG_ERROR ? "ERROR" : level == FETCHKIT_LOG_WARN ? "WARN" : "DEBUG";
    std::fprintf(stderr, "FetchKit [%s] %s\n", name, message);
#endif
}
}

extern "C" fetchkit_status fetchkit_set_log_level(std::uint32_t level) noexcept {
    if (level > FETCHKIT_LOG_DEBUG) return FETCHKIT_INVALID_ARGUMENT;
    log_level.store(level, std::memory_order_relaxed);
    return FETCHKIT_OK;
}

namespace fetchkit::core {
bool diagnosticEnabled(std::uint32_t level) noexcept {
    return level != FETCHKIT_LOG_OFF && level <= log_level.load(std::memory_order_relaxed);
}

void logStatus(std::uint32_t level, const char* message, fetchkit_status status) noexcept {
    if (!diagnosticEnabled(level)) return;
    char text[512];
    std::snprintf(text, sizeof(text), "%s status=%s", message, fetchkit_status_name(status));
    writeLog(level, text);
}

void logTask(std::uint32_t level, const char* message, const fetchkit_task_snapshot& s) noexcept {
    if (!diagnosticEnabled(level)) return;
    char text[768];
    // 只选取核心生成的 ID 与数值字段；调用方须在解锁后传入快照副本。
    std::snprintf(text, sizeof(text),
        "%s taskId=%" PRIu64 " persistentId=%.64s generation=%" PRIu64
        " state=%u status=%s http=%u platform=%d retry=%u nextRetryAtMs=%" PRIu64,
        message, s.task_id, s.persistent_id, s.generation, s.state, fetchkit_status_name(s.error),
        s.http_status, s.platform_code, s.retry_count, s.next_retry_at_ms);
    writeLog(level, text);
}

void logRecovery(std::uint32_t records, std::uint32_t quarantined, std::uint32_t unsupported,
    std::uint32_t orphans, std::uint32_t repaired) noexcept {
    const auto level = quarantined || unsupported || orphans || repaired ? FETCHKIT_LOG_WARN : FETCHKIT_LOG_DEBUG;
    if (!diagnosticEnabled(level)) return;
    char text[512];
    std::snprintf(text, sizeof(text),
        "存储扫描完成 records=%u quarantined=%u unsupported=%u orphans=%u repaired=%u",
        records, quarantined, unsupported, orphans, repaired);
    writeLog(level, text);
}
}
