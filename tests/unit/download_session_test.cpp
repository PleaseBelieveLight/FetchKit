#include "core/engine.h"
#include <fstream>
#include <iostream>

using namespace fetchkit;
using namespace fetchkit::core;

namespace {
void require(bool condition) { if (!condition) throw std::runtime_error("session assertion failed"); }
struct Exchange { int status; std::vector<transport::Header> headers; std::string body; bool stall = false; };
class FakeTransport final : public transport::HttpTransport {
public:
    std::vector<Exchange> exchanges;
    std::vector<transport::Request> requests;
    transport::UrlInfo resolveUrl(const std::string&, const std::string& location) override {
        return {location, "http://fixture:80", false};
    }
    transport::Result execute(const transport::Request& request, transport::Sink& sink, transport::Cancellation& cancellation) override {
        requests.push_back(request);
        require(requests.size() <= exchanges.size());
        const auto& exchange = exchanges[requests.size() - 1];
        if (exchange.status == 0) return {transport::EndReason::Timeout, 0};
        if (!sink.onHeaders({exchange.status, exchange.headers, request.url}, cancellation)) return {transport::EndReason::Rejected, 0};
        if (!sink.onData(reinterpret_cast<const std::uint8_t*>(exchange.body.data()), exchange.body.size(), cancellation)) return {transport::EndReason::Rejected, 0};
        if (exchange.stall) cancellation.waitFor(std::chrono::seconds(5));
        return {cancellation.isCancelled() ? transport::EndReason::Cancelled : transport::EndReason::Complete, 0};
    }
};

fetchkit_task_snapshot snapshot(const std::shared_ptr<Task>& task, bool wait = true) {
    std::unique_lock<std::mutex> lock(task->mutex);
    if (wait) require(task->changed.wait_for(lock, std::chrono::seconds(5), [&] { return stoppedState(task->snapshot.state); }));
    return task->snapshot;
}

Exchange full(std::string body, std::string etag = "\"v1\"", bool stall = false) {
    return {200, {{"Content-Length", "16"}, {"ETag", etag}}, std::move(body), stall};
}

void runCase(const std::string& root, int mode) {
    EngineConfig config; config.root = root; config.max_bytes = 32; config.allow_http = true;
    config.capacity_bytes=4*1024*1024;config.staging_retention_ms=86400000;config.max_retries=0;
    auto transport = std::make_shared<FakeTransport>();
    const std::string content = "0123456789abcdef";
    const bool resume = (mode >= 1 && mode <= 5) || mode == 8;
    transport->exchanges.push_back(full(resume ? content.substr(0, 4) : content, mode == 2 ? "W/\"v1\"" : "\"v1\"", resume));
    if (mode == 1) transport->exchanges.push_back({206, {{"Content-Length", "12"}, {"Content-Range", "bytes 4-15/16"}, {"ETag", "\"v1\""}}, content.substr(4)});
    if (mode == 2 || mode == 3) transport->exchanges.push_back(full(content));
    if (mode == 4 || mode == 5) {
        transport->exchanges.push_back({mode == 4 ? 206 : 416,
            {{"Content-Range", "bytes 0-15/16"}, {"ETag", "\"v1\""}}, "must-not-append"});
        transport->exchanges.push_back(full(content));
    }
    if (mode == 6) transport->exchanges.front().headers.push_back({"Content-Length", "17"});
    if (mode == 7) transport->exchanges.front().headers.push_back({"Content-Encoding", "gzip"});
    if (mode == 8) transport->exchanges.push_back({0, {}, ""});
    auto engine = std::make_shared<Engine>(config, transport);
    DownloadRequest request; request.resource_namespace = "unit"; request.resource_key = std::to_string(mode);
    request.version = "1"; request.request.url = "http://fixture/resource";
    const auto subscription = engine->enqueue(request, 1);
    const auto task=subscription->task;
    if (resume) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (snapshot(task, false).received_bytes < 4 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        require(engine->control(task, StopReason::Pause) == FETCHKIT_OK);
        require(snapshot(task).state == FETCHKIT_PAUSED);
        require(engine->control(task, StopReason::None, true) == FETCHKIT_OK);
    }
    const auto result = snapshot(task);
    require(result.state == (mode < 6 ? FETCHKIT_COMPLETED : FETCHKIT_FAILED));
    if (mode < 6) {
        std::ifstream file(std::filesystem::u8path(root)/"objects"/(std::string(result.persistent_id)+".obj"), std::ios::binary);
        require(std::string(std::istreambuf_iterator<char>(file), {}) == content);
    } else if (mode == 8) {
        require(result.received_bytes == 4 && result.error == FETCHKIT_TIMEOUT && result.http_status == 0 && result.generation == 2);
    } else require(result.received_bytes == 0 && result.error == FETCHKIT_PROTOCOL_MISMATCH);
    if (mode == 2) {
        for (const auto& header : transport->requests[1].headers) require(header.name != "Range");
    }
    engine->close();
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 2);
        const auto run=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        for (int mode = 0; mode < 9; ++mode) runCase(std::string(argv[1]) + '/' + run + '/' + std::to_string(mode), mode);
        std::cout << "9 fake-transport session cases passed\n";
        return 0;
    } catch (const std::exception&) { std::cerr << "Session unit test failed\n"; return 1; }
}
