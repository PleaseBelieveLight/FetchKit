#pragma once

#include "fetchkit/fetchkit.h"
#include "transport/http_transport.h"

#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>

namespace fetchkit::core {

struct Failure : std::exception {
    fetchkit_status code;
    int platform_code;
    explicit Failure(fetchkit_status value, int platform = 0) : code(value), platform_code(platform) {}
    const char* what() const noexcept override { return "fetchkit_failure"; }
};

struct RequestData {
    std::string url;
    std::vector<transport::Header> headers;
    std::vector<std::string> explicitly_set_headers;
};

struct ProviderContext {
    std::uint32_t reason = FETCHKIT_REQUEST_INITIAL;
    std::uint64_t task_id = 0;
    std::uint64_t generation = 1;
    std::uint32_t http_status = 0;
    std::string previous_origin;
    std::string reference;
};

struct DownloadRequest {
    std::string resource_namespace, resource_key, version, representation_key, reference;
    RequestData request;
    std::optional<std::uint64_t> expected_length;
    std::string expected_sha256;
    bool require_digest = false;
    std::function<void(const ProviderContext&, RequestData&, transport::Cancellation&)> provider;
};

struct EngineConfig {
    std::string root, ca_bundle;
    std::uint64_t max_bytes = 0;
    std::uint32_t connect_timeout_ms = 5000, read_timeout_ms = 15000, provider_timeout_ms = 10000;
    std::uint32_t max_redirects = 5, max_requests = 4096;
    bool allow_http = false;
    std::uint64_t checkpoint_bytes = 4ULL * 1024 * 1024;
    std::uint32_t checkpoint_interval_ms = 1000;
};

inline bool protectedHeader(const std::string& name) {
    const auto lower = transport::lowerAscii(name);
    return lower == "range" || lower == "if-range" || lower == "accept-encoding" ||
        lower == "host" || lower == "content-length" || lower == "transfer-encoding" ||
        lower == "connection" || lower == "proxy-authorization";
}

inline void validateHeaders(const std::vector<transport::Header>& headers) {
    if (headers.size() > 64) throw Failure(FETCHKIT_INVALID_ARGUMENT);
    std::size_t total = 0;
    for (const auto& header : headers) {
        total += header.name.size() + header.value.size();
        if (!transport::validHeader(header) || protectedHeader(header.name) || total > 65536) {
            throw Failure(FETCHKIT_INVALID_ARGUMENT);
        }
    }
}

inline bool stoppedState(std::uint32_t state) {
    return state == FETCHKIT_PAUSED || state == FETCHKIT_TRANSFER_COMPLETE ||
        state == FETCHKIT_FAILED || state == FETCHKIT_TASK_CANCELLED || state == FETCHKIT_COMPLETED;
}

}
