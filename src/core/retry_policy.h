#pragma once
#include "core/download_types.h"

namespace fetchkit::core {
bool retryable(fetchkit_status error, std::uint32_t http_status);
std::uint64_t retryAfter(const std::string& value, std::uint64_t now_ms);
std::uint64_t retryDelay(const EngineConfig& config, std::uint32_t used, std::uint64_t random);
}
