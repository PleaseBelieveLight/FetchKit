#pragma once

#include "fetchkit/fetchkit.h"
#include "transport/http_transport.h"
#include <memory>

namespace fetchkit::core {
// Internal platform injection. Uses the same validation, registry and C ABI.
fetchkit_status createEngineWithTransport(const fetchkit_engine_config* config,
    fetchkit_engine* output, std::shared_ptr<transport::HttpTransport> transport) noexcept;
}
