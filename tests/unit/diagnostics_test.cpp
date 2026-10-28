#include "fetchkit/fetchkit.hpp"
#include "core/engine_factory.h"
#include "core/diagnostics.h"
#include "transport/http_transport.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>

#define CHECK(value) do { if (!(value)) { std::cerr << "check_failed line=" << __LINE__ << '\n'; return 1; } } while (0)

namespace {
class OfflineTransport final : public fetchkit::transport::HttpTransport {
public:
    fetchkit::transport::UrlInfo resolveUrl(const std::string&, const std::string& location) override {
        return {location, "https://example.invalid:443", true};
    }
    fetchkit::transport::Result execute(const fetchkit::transport::Request&, fetchkit::transport::Sink&,
        fetchkit::transport::Cancellation&) override {
        return {fetchkit::transport::EndReason::Network, 917};
    }
};
}

int main(int argc, char** argv) {
    CHECK(argc == 3);
    const std::string mode = argv[1];
    if (mode != "default") {
        const auto level = mode == "error" ? fetchkit::LogLevel::Error :
            mode == "warn" ? fetchkit::LogLevel::Warn : fetchkit::LogLevel::Debug;
        fetchkit::setLogLevel(level);
    }
    // 非法设置不得改变已生效的级别；switch 模式验证运行中重新关闭。
    CHECK(fetchkit_set_log_level(99) == FETCHKIT_INVALID_ARGUMENT);
    if (mode == "switch") {
        fetchkit::core::logStatus(FETCHKIT_LOG_DEBUG, "关闭前诊断", FETCHKIT_OK);
        fetchkit::setLogLevel(fetchkit::LogLevel::Off);
    }
    const auto root = std::filesystem::absolute(argv[2]);
    std::filesystem::create_directories(root / "records");
    std::ofstream(root / "records" / (std::string(64, 'a') + ".rec")) << "record-private-secret";

    fetchkit_engine_config config;
    fetchkit_engine_config_init(&config);
    const auto root_text = root.u8string();
    config.storage_root = fetchkit::stringView(root_text);
    config.max_download_bytes = 1048576;
    config.storage_capacity_bytes = 16777216;
    config.staging_retention_ms = 86400000;
    config.max_retries = 1;
    config.retry_base_delay_ms = config.retry_max_delay_ms = 1;
    fetchkit_engine engine = 0;
    CHECK(fetchkit::core::createEngineWithTransport(&config, &engine, std::make_shared<OfflineTransport>()) == FETCHKIT_OK);
    fetchkit_download_request request;
    fetchkit_download_request_init(&request);
    const std::string ns = "identity-private-secret", key = "object", version = "1";
    const std::string url = "https://example.invalid/file?signature=url-private-secret";
    const std::string name = "Authorization", credential = "Bearer header-private-secret";
    const fetchkit_header header{fetchkit::stringView(name), fetchkit::stringView(credential)};
    request.resource_namespace = fetchkit::stringView(ns);
    request.resource_key = fetchkit::stringView(key);
    request.version = fetchkit::stringView(version);
    request.url = fetchkit::stringView(url);
    request.headers = &header;
    request.header_count = 1;
    fetchkit_task task = 0;
    CHECK(fetchkit_enqueue(engine, &request, &task) == FETCHKIT_OK);
    fetchkit_task_snapshot snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    for (int i = 0; i < 10; ++i) {
        const auto status = fetchkit_task_wait(task, 1000, &snapshot);
        CHECK(status == FETCHKIT_OK || status == FETCHKIT_TIMEOUT);
        if (snapshot.state == FETCHKIT_FAILED) break;
    }
    CHECK(snapshot.state == FETCHKIT_FAILED && snapshot.error == FETCHKIT_NETWORK);
    CHECK(snapshot.retry_count == 1 && snapshot.platform_code == 917);
    CHECK(fetchkit_task_release(task) == FETCHKIT_OK);
    CHECK(fetchkit_engine_close(engine) == FETCHKIT_OK);
    std::cout << "{\"passed\":true}\n";
}
