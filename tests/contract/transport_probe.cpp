#include "transport/http_transport.h"

#include <curl/curl.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

using namespace fetchkit::transport;
using Clock = std::chrono::steady_clock;

namespace {

const char* reasonName(EndReason reason) {
    switch (reason) {
        case EndReason::Complete: return "complete";
        case EndReason::Cancelled: return "cancelled";
        case EndReason::Timeout: return "timeout";
        case EndReason::Network: return "network";
        case EndReason::Rejected: return "rejected";
        case EndReason::ProtocolMismatch: return "protocol_mismatch";
    }
    return "unknown";
}

class ProbeSink final : public Sink {
public:
    explicit ProbeSink(std::string mode) : mode_(std::move(mode)) {}

    bool onHeaders(const Response& response, Cancellation& cancellation) override {
        ++header_calls;
        status = response.status;
        for (const auto& header : response.headers) {
            if (header.name == "x-fetchkit") ++duplicate_headers;
            if (header.name == "content-encoding" && header.value != "identity") return false;
        }
        if (mode_ == "reject") return false;
        if (mode_ == "header-barrier") {
            signal();
            cancellation.waitFor(std::chrono::seconds(10));
            return !cancellation.isCancelled();
        }
        return true;
    }

    bool onData(const std::uint8_t* data, std::size_t size, Cancellation& cancellation) override {
        ++body_calls;
        if (mode_ == "backpressure") {
            signal();
            cancellation.waitFor(std::chrono::seconds(10));
            if (cancellation.isCancelled()) return false;
        }
        for (std::size_t i = 0; i < size; ++i) {
            fingerprint ^= data[i];
            fingerprint *= 1099511628211ULL;
        }
        bytes += size;
        if (mode_ == "blocked-read") signal();
        return true;
    }

    bool awaitReady() {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(5), [this] { return ready_; });
    }

    int status = 0;
    int header_calls = 0;
    int body_calls = 0;
    int duplicate_headers = 0;
    std::uint64_t bytes = 0;
    std::uint64_t fingerprint = 14695981039346656037ULL;

private:
    void signal() {
        std::lock_guard<std::mutex> lock(mutex_);
        ready_ = true;
        condition_.notify_all();
    }
    std::string mode_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool ready_ = false;
};

}

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "Usage: fetchkit_transport_probe URL SCENARIO ITERATIONS\n");
        return 2;
    }
    const std::string mode = argv[2];
    const int iterations = std::atoi(argv[3]);
    if (iterations < 1 || iterations > 1000) return 2;
    const bool cancel_mode = mode == "delayed-headers" || mode == "blocked-read" ||
        mode == "header-barrier" || mode == "backpressure" || mode == "close-race";
    std::fprintf(stderr, "libcurl=%s\n", curl_version_info(CURLVERSION_NOW)->version);
    for (int i = 0; i < iterations; ++i) {
        Cancellation cancellation;
        ProbeSink sink(mode);
        Request request;
        request.url = argv[1];
        request.allow_http = true;
        request.read_timeout_ms = mode == "timeout" ? 250 : 2000;
        if (mode == "range") request.headers.push_back({"Range", "bytes=1024-"});
        Result result;
        bool threw = false;
        std::thread worker([&] {
            try {
                CurlProbeTransport transport;
                result = transport.execute(request, sink, cancellation);
            } catch (...) {
                threw = true;
            }
        });
        bool ready = true;
        double cancellation_ms = -1;
        if (cancel_mode) {
            if (mode == "delayed-headers") {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            } else if (mode != "close-race") {
                ready = sink.awaitReady();
                // For blocked-read, allow the worker to enter its next network read.
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
            const auto start = Clock::now();
            cancellation.cancel();
            cancellation.cancel(); // Idempotence while an operation is active.
            worker.join();
            cancellation_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        } else {
            worker.join();
        }
        std::printf("{\"scenario\":\"%s\",\"iteration\":%d,\"reason\":\"%s\","
            "\"status\":%d,\"bytes\":%llu,\"fingerprint\":\"%016llx\","
            "\"header_calls\":%d,\"body_calls\":%d,\"duplicate_headers\":%d,"
            "\"cancel_ms\":%.3f,\"ready\":%s,\"threw\":%s}\n",
            mode.c_str(), i, reasonName(result.reason), sink.status,
            static_cast<unsigned long long>(sink.bytes), static_cast<unsigned long long>(sink.fingerprint),
            sink.header_calls, sink.body_calls, sink.duplicate_headers, cancellation_ms,
            ready ? "true" : "false", threw ? "true" : "false");
        std::fflush(stdout);
        if (!ready || threw) return 1;
    }
    return 0;
}
