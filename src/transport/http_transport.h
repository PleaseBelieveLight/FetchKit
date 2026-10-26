#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace fetchkit::transport {

struct Header {
    std::string name;
    std::string value;
};

struct Request {
    std::string url;
    std::vector<Header> headers;
    std::uint64_t generation = 1;
    long connect_timeout_ms = 5000;
    long read_timeout_ms = 2000;
    bool allow_http = false;
    std::string ca_bundle;
};

struct UrlInfo {
    std::string url;
    std::string origin;
    bool secure = false;
};

struct Response {
    int status = 0;
    std::vector<Header> headers;
    std::string effective_url;
};

// Cancellation owns no transport resources. It outlives execute() and all callbacks.
class Cancellation {
public:
    void cancel() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_.store(true);
        condition_.notify_all();
    }

    bool isCancelled() const noexcept { return cancelled_.load(); }

    bool waitFor(std::chrono::milliseconds duration) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, duration, [this] { return isCancelled(); });
    }

private:
    std::atomic<bool> cancelled_{false};
    std::mutex mutex_;
    std::condition_variable condition_;
};

enum class EndReason { Complete, Cancelled, Timeout, Network, Rejected, ProtocolMismatch, Certificate };

struct Result {
    EndReason reason = EndReason::Network;
    int platform_code = 0;
};

// Callbacks run on execute's worker, never under an SDK lock.
// Data is borrowed until onData returns. Waiting sinks must observe Cancellation.
class Sink {
public:
    virtual ~Sink() = default;
    virtual bool onHeaders(const Response& response, Cancellation& cancellation) = 0;
    virtual bool onData(const std::uint8_t* data, std::size_t size, Cancellation& cancellation) = 0;
};

class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    // Returning is the quiescence barrier: no later callbacks or buffer accesses.
    virtual Result execute(const Request& request, Sink& sink, Cancellation& cancellation) = 0;
    virtual UrlInfo resolveUrl(const std::string& base, const std::string& location) = 0;
};

class CurlTransport final : public HttpTransport {
public:
    Result execute(const Request& request, Sink& sink, Cancellation& cancellation) override;
    UrlInfo resolveUrl(const std::string& base, const std::string& location) override;
};

inline std::string lowerAscii(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return value;
}

inline bool validHeader(const Header& header) {
    if (header.name.empty() || header.name.size() > 256 || header.value.size() > 8192) return false;
    for (unsigned char c : header.name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) &&
            std::string("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) == std::string::npos) return false;
    }
    for (unsigned char c : header.value) if (c == 127 || (c < 32 && c != '\t')) return false;
    return true;
}

}
