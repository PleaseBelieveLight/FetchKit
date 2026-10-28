#include "core/engine.h"
#include "storage/fault.h"
#include "core/retry_policy.h"
#include "core/diagnostics.h"

#include <algorithm>
#include <set>

namespace fetchkit::core {
namespace {

std::optional<std::string> oneHeader(const transport::Response& response, const std::string& name) {
    std::optional<std::string> value;
    for (const auto& header : response.headers) {
        if (transport::lowerAscii(header.name) == name) {
            if (value) throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
            value = header.value;
        }
    }
    return value;
}

std::uint64_t number(const std::string& value) {
    if (value.empty()) throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
    std::uint64_t result = 0;
    for (unsigned char c : value) {
        if (c < '0' || c > '9' || result > (static_cast<std::uint64_t>(INT64_MAX) - (c - '0')) / 10) {
            throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
        }
        result = result * 10 + (c - '0');
    }
    return result;
}

std::optional<std::uint64_t> contentLength(const transport::Response& response) {
    std::optional<std::uint64_t> result;
    for (const auto& header : response.headers) {
        if (transport::lowerAscii(header.name) != "content-length") continue;
        const auto length = number(header.value);
        if (result && *result != length) throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
        result = length;
    }
    if (oneHeader(response, "transfer-encoding") && result) throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
    return result;
}

std::string strongEtag(const transport::Response& response) {
    const auto value = oneHeader(response, "etag");
    if (!value || value->size() < 2 || value->size() > 1024 || value->front() != '"' || value->back() != '"') return {};
    for (std::size_t i = 1; i + 1 < value->size(); ++i) {
        const auto c = static_cast<unsigned char>((*value)[i]);
        if (c < 33 || c == '"' || c == 127) return {};
    }
    return *value;
}

struct ByteRange { std::uint64_t first, last, total; };

ByteRange contentRange(const transport::Response& response) {
    const auto value = oneHeader(response, "content-range");
    if (!value || value->rfind("bytes ", 0) != 0) throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
    const auto dash = value->find('-', 6), slash = value->find('/', 6);
    if (dash == std::string::npos || slash == std::string::npos || dash >= slash) throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
    ByteRange range{number(value->substr(6, dash - 6)), number(value->substr(dash + 1, slash - dash - 1)), number(value->substr(slash + 1))};
    if (range.last < range.first || range.last >= range.total) throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
    return range;
}

class DownloadSession final : public transport::Sink {
public:
    DownloadSession(Task& task, const EngineConfig& config, transport::HttpTransport& transport)
        : task_(task), config_(config), transport_(transport), file_(task.path, false), cancellation_(*task.cancellation) {}

    RunResult run() {
        try {
            if (file_.size() != task_.saved.offset) throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
            if (task_.saved.offset && task_.saved.strong_etag.empty()) reset("缺少强 ETag，改为从头下载");
            request_ = task_.request.request;
            prepare(FETCHKIT_REQUEST_INITIAL, {}, 0);
            std::set<std::string> visited{current_.url};
            std::uint32_t redirects = 0;
            for (std::uint32_t count = 0; count < config_.max_requests; ++count) {
                if (cancellation_.isCancelled()) throw Failure(FETCHKIT_CANCELLED);
                request_offset_ = task_.saved.offset;
                expected_body_.reset();
                body_received_ = 0;
                action_ = Action::None;
                error_ = {};
                transport::Request request;
                request.url = current_.url;
                request.headers = request_.headers;
                request.generation = task_.snapshot.generation;
                request.allow_http = config_.allow_http;
                request.ca_bundle = config_.ca_bundle;
                request.connect_timeout_ms = config_.connect_timeout_ms;
                request.read_timeout_ms = config_.read_timeout_ms;
                if (request_offset_) {
                    request.headers.push_back({"Range", "bytes=" + std::to_string(request_offset_) + "-"});
                    request.headers.push_back({"If-Range", task_.saved.strong_etag});
                }
                {
                    std::lock_guard<std::mutex> lock(task_.mutex);
                    if(task_.snapshot.request_count>=config_.max_requests) throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
                    ++task_.snapshot.request_count;
                }
                const auto result = transport_.execute(request, *this, cancellation_);
                if (cancellation_.isCancelled()) throw Failure(FETCHKIT_CANCELLED);
                if (error_.error) throw Failure(error_.error, error_.platform_code);
                if (action_ == Action::Restart) {
                    if (restarts_++ != 0) throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
                    reset("续传响应不符合约束，改为完整重下");
                    visited.clear(); visited.insert(current_.url);
                    continue;
                }
                if (action_ == Action::Redirect) {
                    if (++redirects > config_.max_redirects) throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
                    const auto old = current_;
                    auto next = transport_.resolveUrl(current_.url, location_);
                    if (old.secure && !next.secure) throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
                    request_.url = next.url;
                    if (next.origin != old.origin) request_.headers.clear();
                    prepare(FETCHKIT_REQUEST_REDIRECT, old.origin, response_status_);
                    if (old.secure && !current_.secure) throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
                    if (!visited.insert(current_.url).second) throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
                    continue;
                }
                if (action_ == Action::Refresh) {
                    if (!task_.request.provider || task_.record.auth_refreshes != 0) throw Failure(FETCHKIT_AUTH_REQUIRED);
                    ++task_.record.auth_refreshes;
                    if(const auto store=task_.store.lock()) store->save(task_.record);
                    const auto old = current_;
                    prepare(FETCHKIT_REQUEST_AUTH_REFRESH, old.origin, response_status_);
                    if (old.secure && !current_.secure) throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
                    continue;
                }
                switch (result.reason) {
                    case transport::EndReason::Complete: break;
                    case transport::EndReason::Timeout: throw Failure(FETCHKIT_TIMEOUT, result.platform_code);
                    case transport::EndReason::Certificate: throw Failure(FETCHKIT_CERTIFICATE_ERROR, result.platform_code);
                    case transport::EndReason::Network: throw Failure(FETCHKIT_NETWORK, result.platform_code);
                    default: throw Failure(FETCHKIT_PROTOCOL_MISMATCH, result.platform_code);
                }
                if (expected_body_ && body_received_ != *expected_body_) throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
                if (response_status_ == 206 && task_.saved.total && task_.saved.offset < *task_.saved.total) {
                    visited.clear(); visited.insert(current_.url); redirects = 0;
                    continue;
                }
                if (task_.saved.total && task_.saved.offset != *task_.saved.total) throw Failure(FETCHKIT_INTEGRITY_MISMATCH);
                checkpoint();
                return {};
            }
            throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
        } catch (const Failure& error) {
            try {
                task_.saved.offset = file_.size();
                if(const auto store=task_.store.lock()) store->observeSize(task_.record.id,task_.saved.offset);
                task_.progress(task_.saved.offset, task_.saved.total);
                if (task_.stop.load() == StopReason::Pause) {
                    std::lock_guard<std::mutex> lock(task_.mutex);
                    task_.snapshot.state = FETCHKIT_PAUSING;
                }
                if (task_.stop.load() != StopReason::Cancel && !checkpoint_failed_) checkpoint();
            } catch (const Failure& storage_error) { return {storage_error.code, storage_error.platform_code}; }
            return {error.code, error.platform_code};
        } catch (const std::invalid_argument&) { return {FETCHKIT_PROTOCOL_MISMATCH, 0}; }
    }

    bool onHeaders(const transport::Response& response, transport::Cancellation&) override {
        try {
            response_status_ = response.status;
            {
                std::lock_guard<std::mutex> lock(task_.mutex);
                task_.snapshot.http_status = static_cast<std::uint32_t>(response.status);
            }
            if (response.status == 301 || response.status == 302 || response.status == 303 || response.status == 307 || response.status == 308) {
                const auto location = oneHeader(response, "location");
                if (!location || location->empty()) throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
                location_ = *location; action_ = Action::Redirect; return false;
            }
            if (response.status == 401 || response.status == 403) { action_ = Action::Refresh; return false; }
            if (response.status == 416) {
                if (!request_offset_) throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
                action_ = Action::Restart; return false;
            }
            if (response.status != 200 && response.status != 206) {
                if(const auto retry=oneHeader(response,"retry-after")) task_.retry_after_ms=retryAfter(*retry,config_.clock->nowMs());
                throw Failure(FETCHKIT_HTTP_STATUS);
            }
            const auto encoding = oneHeader(response, "content-encoding");
            if (encoding && transport::lowerAscii(*encoding) != "identity") throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
            const auto length = contentLength(response);
            if (response.status == 200) {
                if (oneHeader(response, "content-range")) throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
                if (length && *length > config_.max_bytes) throw Failure(FETCHKIT_QUOTA_EXCEEDED);
                if (length && task_.request.expected_length && *length != *task_.request.expected_length) throw Failure(FETCHKIT_INTEGRITY_MISMATCH);
                if (request_offset_) reset("服务端返回完整响应，丢弃旧前缀后重新下载");
                task_.saved.total = length ? length : task_.request.expected_length;
                task_.saved.strong_etag = strongEtag(response);
                expected_body_ = length;
            } else {
                try {
                    const auto range = contentRange(response);
                    if (!request_offset_ || range.first != request_offset_ ||
                        task_.saved.strong_etag.empty() || strongEtag(response) != task_.saved.strong_etag ||
                        (task_.saved.total && range.total != *task_.saved.total) ||
                        (task_.request.expected_length && range.total != *task_.request.expected_length) ||
                        (length && *length != range.last - range.first + 1)) throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
                    if (range.total > config_.max_bytes) throw Failure(FETCHKIT_QUOTA_EXCEEDED);
                    task_.saved.total = range.total;
                    expected_body_ = range.last - range.first + 1;
                } catch (const Failure& error) {
                    if (error.code != FETCHKIT_PROTOCOL_MISMATCH || !request_offset_) throw;
                    action_ = Action::Restart; return false;
                }
            }
            if(const auto store=task_.store.lock()) store->reserve(task_.record.id,task_.saved.offset,task_.saved.total);
            file_.seek(task_.saved.offset);
            task_.progress(task_.saved.offset, task_.saved.total);
            return true;
        } catch (const Failure& error) { error_ = {error.code, error.platform_code}; return false; }
    }

    bool onData(const std::uint8_t* data, std::size_t size, transport::Cancellation& cancellation) override {
        try {
            if (cancellation.isCancelled()) return false;
            if (size > config_.max_bytes - task_.saved.offset) throw Failure(FETCHKIT_QUOTA_EXCEEDED);
            if (expected_body_ && size > *expected_body_ - body_received_) throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
            if (task_.saved.total && (task_.saved.offset > *task_.saved.total ||
                size > *task_.saved.total - task_.saved.offset)) throw Failure(FETCHKIT_INTEGRITY_MISMATCH);
            if(const auto store=task_.store.lock()) store->reserve(task_.record.id,task_.saved.offset+size,task_.saved.total);
            storage::faultPoint("data_write_before");
            file_.write(data, size);
            storage::faultPoint("data_write_after");
            task_.saved.offset += size;
            if(const auto store=task_.store.lock()) store->observeSize(task_.record.id,task_.saved.offset);
            body_received_ += size;
            task_.progress(task_.saved.offset, task_.saved.total);
            if (task_.saved.offset-task_.record.committed>=config_.checkpoint_bytes ||
                std::chrono::steady_clock::now()-last_checkpoint_>=std::chrono::milliseconds(config_.checkpoint_interval_ms)) checkpoint();
            return true;
        } catch (const Failure& error) { error_ = {error.code, error.platform_code}; return false; }
    }

private:
    enum class Action { None, Restart, Redirect, Refresh };
    void checkpoint() {
        try {
            // 先同步数据，再原子提交记录；只有两者成功后才能对外推进 committed_offset。
            storage::faultPoint("checkpoint_data_sync_before");
            file_.sync();
            storage::faultPoint("checkpoint_data_sync_after");
            auto record=task_.record;
            record.committed=task_.saved.offset; record.total=task_.saved.total;
            record.etag=task_.saved.strong_etag; record.state=FETCHKIT_RUNNING;
            record.generation=task_.snapshot.generation; record.integrity=FETCHKIT_INTEGRITY_NONE; record.error=FETCHKIT_OK;
            const auto store=task_.store.lock();
            if (!store) throw Failure(FETCHKIT_CLOSED);
            store->save(record);
            task_.record=std::move(record);
            { std::lock_guard<std::mutex> lock(task_.mutex); task_.snapshot.committed_offset=task_.record.committed; }
            last_checkpoint_=std::chrono::steady_clock::now();
            storage::faultPoint("checkpoint_commit_after");
        } catch (...) { checkpoint_failed_=true; throw; }
    }
    void reset(const char* reason) {
        // 覆盖旧数据前先提交零偏移，避免中断后把旧 ETag/偏移与新内容配对。
        task_.saved = {0, task_.request.expected_length, {}};
        checkpoint();
        storage::faultPoint("reset_commit_after");
        file_.truncate();
        file_.sync();
        if(const auto store=task_.store.lock()) store->observeSize(task_.record.id,0);
        task_.progress(0, task_.saved.total);
        if (!reset_logged_) {
            reset_logged_ = true;
            fetchkit_task_snapshot diagnostic;
            { std::lock_guard<std::mutex> lock(task_.mutex); diagnostic = task_.snapshot; }
            logTask(FETCHKIT_LOG_WARN, reason, diagnostic);
        }
    }
    void prepare(std::uint32_t reason, const std::string& previous_origin, std::uint32_t status) {
        if (cancellation_.isCancelled()) throw Failure(FETCHKIT_CANCELLED);
        std::optional<transport::UrlInfo> proposed;
        if (!request_.url.empty()) proposed = transport_.resolveUrl("", request_.url);
        request_.explicitly_set_headers.clear();
        request_.origin_before_provider.clear();
        if (task_.request.provider) {
            ProviderContext context{reason, task_.snapshot.task_id, task_.snapshot.generation, status, previous_origin, task_.request.reference};
            const auto start = std::chrono::steady_clock::now();
            task_.request.provider(context, request_, cancellation_);
            if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(config_.provider_timeout_ms)) throw Failure(FETCHKIT_TIMEOUT);
        }
        if (cancellation_.isCancelled()) throw Failure(FETCHKIT_CANCELLED);
        if(!request_.origin_before_provider.empty()) proposed=transport_.resolveUrl("",request_.origin_before_provider);
        validateHeaders(request_.headers);
        current_ = transport_.resolveUrl("", request_.url);
        if (!current_.secure && !config_.allow_http) throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
        if (proposed && proposed->secure && !current_.secure) throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
        if (proposed && proposed->origin != current_.origin) {
            // 跨 origin 只保留本轮 provider 显式提供的字段，防止把旧站点凭证转交给新站点。
            std::vector<transport::Header> explicit_headers;
            for (const auto& header : request_.headers) {
                const auto name = transport::lowerAscii(header.name);
                if (std::find(request_.explicitly_set_headers.begin(), request_.explicitly_set_headers.end(), name) != request_.explicitly_set_headers.end()) {
                    explicit_headers.push_back(header);
                }
            }
            request_.headers = std::move(explicit_headers);
        }
    }
    Task& task_;
    const EngineConfig& config_;
    transport::HttpTransport& transport_;
    storage::StagingFile file_;
    transport::Cancellation& cancellation_;
    RequestData request_;
    transport::UrlInfo current_;
    std::uint64_t request_offset_ = 0, body_received_ = 0;
    std::optional<std::uint64_t> expected_body_;
    std::uint32_t restarts_ = 0;
    int response_status_ = 0;
    Action action_ = Action::None;
    std::string location_;
    RunResult error_;
    bool checkpoint_failed_=false;
    bool reset_logged_=false;
    std::chrono::steady_clock::time_point last_checkpoint_=std::chrono::steady_clock::now();
};

}

RunResult runDownload(Task& task, const EngineConfig& config, transport::HttpTransport& transport) {
    try { DownloadSession session(task, config, transport); return session.run(); }
    catch (const Failure& error) { return {error.code, error.platform_code}; }
}

}
