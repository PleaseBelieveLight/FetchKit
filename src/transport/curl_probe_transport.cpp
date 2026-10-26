#include "transport/http_transport.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace fetchkit::transport {
namespace {

using Clock = std::chrono::steady_clock;

struct CurlGlobal {
    CurlGlobal() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            throw std::runtime_error("curl initialization failed");
        }
    }
    ~CurlGlobal() { curl_global_cleanup(); }
};

struct Context {
    Sink& sink;
    Cancellation& cancellation;
    Response response;
    bool accepted = false;
    bool rejected = false;
    bool invalid = false;
    std::size_t header_bytes = 0;
    Clock::time_point activity = Clock::now();
};

std::string trim(std::string value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::size_t receiveHeader(char* data, std::size_t size, std::size_t count, void* user) noexcept {
    auto& context = *static_cast<Context*>(user);
    const std::size_t length = size * count;
    try {
        if (context.cancellation.isCancelled()) return 0;
        context.activity = Clock::now();
        context.header_bytes += length;
        if (context.header_bytes > 65536) {
            context.invalid = true;
            return 0;
        }
        const std::string line(data, length);
        if (line.rfind("HTTP/", 0) == 0) {
            if (context.accepted) {
                context.invalid = true;
                return 0;
            }
            context.response.headers.clear();
            if (std::sscanf(line.c_str(), "HTTP/%*s %d", &context.response.status) != 1) {
                context.invalid = true;
                return 0;
            }
        } else if (line == "\r\n") {
            if (!context.accepted && context.response.status >= 200) {
                context.accepted = context.sink.onHeaders(context.response, context.cancellation);
                context.rejected = !context.accepted;
                if (!context.accepted) return 0;
            }
        } else {
            // Trailers do not replace already accepted response headers.
            if (context.accepted) return length;
            const auto colon = line.find(':');
            if (colon == std::string::npos || context.response.headers.size() >= 256) {
                context.invalid = true;
                return 0;
            }
            auto name = line.substr(0, colon);
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            context.response.headers.push_back({name, trim(line.substr(colon + 1))});
        }
        return length;
    } catch (...) {
        context.invalid = true;
        return 0;
    }
}

std::size_t receiveBody(char* data, std::size_t size, std::size_t count, void* user) noexcept {
    auto& context = *static_cast<Context*>(user);
    try {
        if (context.cancellation.isCancelled() || !context.accepted) return 0;
        context.activity = Clock::now();
        const auto length = size * count;
        if (!context.sink.onData(reinterpret_cast<std::uint8_t*>(data), length, context.cancellation)) {
            context.rejected = true;
            return 0;
        }
        context.activity = Clock::now();
        return length;
    } catch (...) {
        context.invalid = true;
        return 0;
    }
}

}

Result CurlProbeTransport::execute(const Request& request, Sink& sink, Cancellation& cancellation) {
    if (cancellation.isCancelled()) return {EndReason::Cancelled, 0};
    if ((!request.allow_http && request.url.rfind("https://", 0) != 0) ||
        request.connect_timeout_ms <= 0 || request.read_timeout_ms <= 0) {
        return {EndReason::ProtocolMismatch, 0};
    }
    static CurlGlobal global;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> easy(curl_easy_init(), curl_easy_cleanup);
    std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> multi(curl_multi_init(), curl_multi_cleanup);
    if (!easy || !multi) return {EndReason::Network, CURLE_OUT_OF_MEMORY};
    Context context{sink, cancellation, {0, {}, request.url}};
    curl_slist* raw_headers = nullptr;
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(nullptr, curl_slist_free_all);
    for (const auto& header : request.headers) {
        auto* updated = curl_slist_append(raw_headers, (header.name + ": " + header.value).c_str());
        if (!updated) return {EndReason::Network, CURLE_OUT_OF_MEMORY};
        headers.release();
        raw_headers = updated;
        headers.reset(raw_headers);
    }
    const auto set = [&](CURLoption option, auto value) {
        if (curl_easy_setopt(easy.get(), option, value) != CURLE_OK) {
            throw std::runtime_error("curl option unavailable");
        }
    };
    set(CURLOPT_URL, request.url.c_str());
    set(CURLOPT_PROTOCOLS_STR, request.allow_http ? "http,https" : "https");
    set(CURLOPT_FOLLOWLOCATION, 0L);
    set(CURLOPT_PROXY, ""); // Probe is deterministic and does not inherit proxy credentials.
    set(CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_NONE));
    set(CURLOPT_ACCEPT_ENCODING, "identity");
    set(CURLOPT_HTTP_CONTENT_DECODING, 0L);
    set(CURLOPT_CONNECTTIMEOUT_MS, request.connect_timeout_ms);
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_FORBID_REUSE, 1L);
    set(CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
    set(CURLOPT_HTTPHEADER, headers.get());
    set(CURLOPT_HEADERFUNCTION, &receiveHeader);
    set(CURLOPT_HEADERDATA, &context);
    set(CURLOPT_WRITEFUNCTION, &receiveBody);
    set(CURLOPT_WRITEDATA, &context);
    if (curl_multi_add_handle(multi.get(), easy.get()) != CURLM_OK) return {EndReason::Network, 0};
    int running = 1;
    CURLcode code = CURLE_OK;
    bool timeout = false;
    bool multi_failed = false;
    while (running && !cancellation.isCancelled()) {
        if (curl_multi_perform(multi.get(), &running) != CURLM_OK) {
            multi_failed = true;
            break;
        }
        if (!running || cancellation.isCancelled()) break;
        if (Clock::now() - context.activity >= std::chrono::milliseconds(request.read_timeout_ms)) {
            timeout = true;
            break;
        }
        if (curl_multi_poll(multi.get(), nullptr, 0, 50, nullptr) != CURLM_OK) {
            multi_failed = true;
            break;
        }
    }
    int remaining = 0;
    while (auto* message = curl_multi_info_read(multi.get(), &remaining)) {
        if (message->msg == CURLMSG_DONE) code = message->data.result;
    }
    curl_multi_remove_handle(multi.get(), easy.get());
    // Local RAII cleanup completes before the caller sees a result.
    if (cancellation.isCancelled()) return {EndReason::Cancelled, static_cast<int>(code)};
    if (context.invalid) return {EndReason::ProtocolMismatch, static_cast<int>(code)};
    if (context.rejected) return {EndReason::Rejected, static_cast<int>(code)};
    if (timeout || code == CURLE_OPERATION_TIMEDOUT) return {EndReason::Timeout, static_cast<int>(code)};
    if (multi_failed || code != CURLE_OK) return {EndReason::Network, static_cast<int>(code)};
    if (!context.accepted) return {EndReason::ProtocolMismatch, 0};
    return {EndReason::Complete, 0};
}

}
