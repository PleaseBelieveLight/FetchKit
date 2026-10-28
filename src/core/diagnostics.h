#pragma once

#include "fetchkit/fetchkit.h"
#include <cstdint>

namespace fetchkit::core {

// 仅在操作边界输出；message 必须为固定文本，禁止传入 URL、凭证或异常原文。
bool diagnosticEnabled(std::uint32_t level) noexcept;
void logStatus(std::uint32_t level, const char* message, fetchkit_status status) noexcept;
void logTask(std::uint32_t level, const char* message, const fetchkit_task_snapshot& snapshot) noexcept;
void logRecovery(std::uint32_t records, std::uint32_t quarantined, std::uint32_t unsupported,
    std::uint32_t orphans, std::uint32_t repaired) noexcept;

}
