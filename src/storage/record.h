#pragma once
#include "core/download_types.h"
#include <vector>

namespace fetchkit::storage {
struct Record {
    std::string id;
    core::DownloadRequest request; // Only identity, reference and constraints are encoded.
    std::uint64_t committed = 0, generation = 1;
    std::optional<std::uint64_t> total;
    std::string etag;
    std::uint32_t state = FETCHKIT_PAUSED, integrity = 0;
    fetchkit_status error = FETCHKIT_OK;
    std::uint32_t format_version = 1, retry_count = 0, auth_refreshes = 0;
    std::uint64_t next_retry_at_ms = 0, updated_at_ms = 0, access_order = 0, expires_at_ms = 0;
    bool pinned = false;
};
std::string resourceId(const core::DownloadRequest& request);
bool validId(const std::string& id);
std::vector<std::uint8_t> encodeRecord(const Record& record);
Record decodeRecord(const std::vector<std::uint8_t>& bytes);
}
