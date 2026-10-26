#include "fetchkit/fetchkit.hpp"
#include "snapshot_json.h"

#include <atomic>
#include <iostream>
#include <thread>

struct Provider {
    std::string base, scenario, peer;
    int calls = 0, refreshes = 0, redirects = 0;
    bool reentry_ok = true;
    fetchkit_engine engine = 0;
    std::atomic<bool> entered{false};
};

fetchkit_status provide(void* raw, const fetchkit_request_context* context, fetchkit_request_builder* builder) {
    auto& provider = *static_cast<Provider*>(raw);
    provider.entered.store(true);
    ++provider.calls;
    fetchkit_task_snapshot snapshot{}; snapshot.struct_size = sizeof(snapshot);
    provider.reentry_ok = provider.reentry_ok && fetchkit_task_query(context->task_id, &snapshot) == FETCHKIT_OK;
    provider.reentry_ok = provider.reentry_ok && fetchkit_task_wait(context->task_id, 1, &snapshot) == FETCHKIT_BUSY;
    provider.reentry_ok = provider.reentry_ok && fetchkit_engine_close(provider.engine) == FETCHKIT_BUSY;
    if (context->reason == FETCHKIT_REQUEST_AUTH_REFRESH) {
        ++provider.refreshes;
        if (provider.scenario != "auth-fail") {
            static const std::string name = "Authorization", value = "Bearer synthetic-refreshed";
            return fetchkit_request_set_header(builder, fetchkit::stringView(name), fetchkit::stringView(value));
        }
    }
    if (context->reason == FETCHKIT_REQUEST_REDIRECT) ++provider.redirects;
    if (provider.scenario == "provider-cross-origin") {
        const std::string before(context->proposed_url.data, static_cast<std::size_t>(context->proposed_url.size));
        const auto url = provider.peer + "/m1/normal";
        const auto status = fetchkit_request_set_url(builder, fetchkit::stringView(url));
        provider.reentry_ok = provider.reentry_ok && before == std::string(context->proposed_url.data, static_cast<std::size_t>(context->proposed_url.size));
        return status;
    }
    if (provider.scenario == "provider-cancel") {
        while (!fetchkit_request_is_cancelled(builder)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return FETCHKIT_CANCELLED;
    }
    if (provider.scenario == "provider-throws") throw std::runtime_error("synthetic-secret-must-not-leak");
    return FETCHKIT_OK;
}

int main(int argc, char** argv) {
    if (argc != 6) return 2;
    const std::string base = argv[1], scenario = argv[2], root = argv[3], ca = argv[4];
    fetchkit_engine_config config{}; fetchkit_engine_config_init(&config);
    config.storage_root = fetchkit::stringView(root);
    config.ca_bundle = fetchkit::stringView(ca);
    config.allow_http = 1;
    config.max_download_bytes = scenario == "unknown-limit" ? 4096 : 8ULL * 1024 * 1024 * 1024;
    config.read_timeout_ms = scenario == "timeout" ? 150 : 3000;
    config.max_requests = 64;
    Provider provider{base, scenario, argv[5]};
    fetchkit_engine engine = 0;
    const auto created = fetchkit_engine_create(&config, &engine);
    if (created != FETCHKIT_OK) { std::cout << "{\"create_error\":" << jsonString(fetchkit_status_name(created)) << "}\n"; return 0; }
    provider.engine = engine;
    std::string url = base + "/m1/" + scenario;
    const std::string ns = "protocol", version = "1", reference = "business-reference";
    fetchkit_download_request request{}; fetchkit_download_request_init(&request);
    request.resource_namespace = fetchkit::stringView(ns); request.resource_key = fetchkit::stringView(scenario);
    request.version = fetchkit::stringView(version); request.url = fetchkit::stringView(url);
    request.address_reference = fetchkit::stringView(reference);
    request.provider = provide; request.provider_user_data = &provider;
    const std::string auth = "Authorization", cookie = "Cookie", api = "X-Api-Key", synthetic = "synthetic-secret";
    const fetchkit_header headers[] = {{fetchkit::stringView(auth), fetchkit::stringView(synthetic)},
        {fetchkit::stringView(cookie), fetchkit::stringView(synthetic)}, {fetchkit::stringView(api), fetchkit::stringView(synthetic)}};
    if (scenario == "cross-origin" || scenario == "same-origin" || scenario == "provider-cross-origin") { request.headers = headers; request.header_count = 3; }
    if (scenario == "length-mismatch") { request.expected_length_known = 1; request.expected_length = 100; }
    if (scenario == "resume-complete-416") { request.expected_length_known = 1; request.expected_length = 262144; }
    fetchkit_task id = 0;
    const auto enqueued = fetchkit_enqueue(engine, &request, &id);
    if (enqueued != FETCHKIT_OK) {
        fetchkit_engine_close(engine);
        std::cout << "{\"enqueue_error\":" << jsonString(fetchkit_status_name(enqueued)) << "}\n"; return 0;
    }
    fetchkit::Task task(id);
    bool resumed = false;
    std::uint64_t paused_bytes = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    fetchkit_task_snapshot result{};
    while (std::chrono::steady_clock::now() < deadline) {
        result = task.query();
        const bool should_pause = (scenario.rfind("resume-", 0) == 0 && scenario != "resume-on-error") || scenario == "weak-etag" || scenario == "no-etag" || scenario == "last-modified";
        const auto threshold = scenario == "resume-complete-416" ? 262144ULL : 1024ULL;
        if (!resumed && should_pause && result.state == FETCHKIT_RUNNING && result.received_bytes >= threshold) {
            if (task.pause() == FETCHKIT_OK) {
                result = task.wait(60000);
                paused_bytes = result.received_bytes;
                if (result.state != FETCHKIT_PAUSED || task.resume() != FETCHKIT_OK) return 3;
                resumed = true; continue;
            }
        }
        if (!resumed && scenario == "resume-on-error" && result.state == FETCHKIT_FAILED) {
            paused_bytes = result.received_bytes;
            if (task.resume() != FETCHKIT_OK) return 3;
            resumed = true; continue;
        }
        if (scenario == "cancel" && result.received_bytes >= 1024) task.cancel();
        if (scenario == "provider-cancel" && provider.entered.load()) task.cancel();
        if (result.state == FETCHKIT_TRANSFER_COMPLETE || result.state == FETCHKIT_FAILED || result.state == FETCHKIT_TASK_CANCELLED) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (result.state == FETCHKIT_RUNNING || result.state == FETCHKIT_QUEUED || result.state == FETCHKIT_PAUSING) return 4;
    fetchkit_engine_close(engine);
    const auto closed_resume = fetchkit_task_resume(id);
    const auto released = fetchkit_task_release(id);
    fetchkit_task_snapshot stale{}; stale.struct_size = sizeof(stale);
    const auto stale_query = fetchkit_task_query(id, &stale);
    std::cout << "{\"snapshot\":"; printSnapshot(std::cout, result);
    std::cout << ",\"paused_bytes\":" << paused_bytes << ",\"provider_calls\":" << provider.calls
        << ",\"refreshes\":" << provider.refreshes << ",\"redirects\":" << provider.redirects
        << ",\"closed_resume\":" << jsonString(fetchkit_status_name(closed_resume))
        << ",\"release\":" << jsonString(fetchkit_status_name(released))
        << ",\"stale_query\":" << jsonString(fetchkit_status_name(stale_query))
        << ",\"reentry_ok\":" << (provider.reentry_ok ? "true" : "false") << "}\n";
    return 0;
}
