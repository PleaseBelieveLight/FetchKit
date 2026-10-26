#include "fetchkit/fetchkit.h"
#include "core/engine.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

struct fetchkit_request_builder {
    fetchkit::core::RequestData* data;
    fetchkit::transport::Cancellation* cancellation;
    fetchkit_status error = FETCHKIT_OK;
};

namespace {
using namespace fetchkit;
using namespace fetchkit::core;

struct Registry {
    std::mutex mutex;
    std::uint64_t next_id = 1;
    std::unordered_map<std::uint64_t, std::shared_ptr<Engine>> engines;
    std::unordered_map<std::uint64_t, std::shared_ptr<Task>> tasks;
    std::uint64_t allocate() {
        std::lock_guard<std::mutex> lock(mutex);
        if (next_id == UINT64_MAX) throw Failure(FETCHKIT_BUSY);
        return next_id++;
    }
};
Registry& registry() { static Registry value; return value; }

template<class F> fetchkit_status boundary(F function) noexcept {
    try { return function(); }
    catch (const Failure& error) { return error.code; }
    catch (const std::bad_alloc&) { return FETCHKIT_OUT_OF_MEMORY; }
    catch (...) { return FETCHKIT_INTERNAL_ERROR; }
}

std::string text(fetchkit_string value, std::size_t maximum, bool required = false) {
    if (value.size > maximum || (value.size && !value.data) || (required && !value.size)) throw Failure(FETCHKIT_INVALID_ARGUMENT);
    std::string result(value.data ? value.data : "", static_cast<std::size_t>(value.size));
    // Strict UTF-8: reject NUL, overlong forms, surrogate code points and > U+10FFFF.
    for (std::size_t i = 0; i < result.size();) {
        auto c = static_cast<unsigned char>(result[i++]);
        if (c == 0) throw Failure(FETCHKIT_INVALID_ARGUMENT);
        if (c < 128) continue;
        unsigned int count = 0, scalar = 0, minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) { count = 1; scalar = c & 31; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { count = 2; scalar = c & 15; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 3; scalar = c & 7; minimum = 0x10000; }
        else throw Failure(FETCHKIT_INVALID_ARGUMENT);
        while (count--) {
            if (i >= result.size()) throw Failure(FETCHKIT_INVALID_ARGUMENT);
            c = static_cast<unsigned char>(result[i++]);
            if ((c & 0xc0) != 0x80) throw Failure(FETCHKIT_INVALID_ARGUMENT);
            scalar = (scalar << 6) | (c & 63);
        }
        if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) throw Failure(FETCHKIT_INVALID_ARGUMENT);
    }
    return result;
}

fetchkit_string view(const std::string& value) { return {value.data(), static_cast<std::uint64_t>(value.size())}; }

std::shared_ptr<Engine> engineFor(fetchkit_engine id) {
    std::lock_guard<std::mutex> lock(registry().mutex);
    const auto it = registry().engines.find(id);
    if (it == registry().engines.end()) throw Failure(FETCHKIT_CLOSED);
    return it->second;
}

std::shared_ptr<Task> taskFor(fetchkit_task id) {
    std::lock_guard<std::mutex> lock(registry().mutex);
    const auto it = registry().tasks.find(id);
    if (it == registry().tasks.end() || !it->second) throw Failure(FETCHKIT_CLOSED);
    return it->second;
}

fetchkit_status control(fetchkit_task id, StopReason reason, bool resume = false) {
    const auto task = taskFor(id);
    const auto engine = task->owner.lock();
    return engine ? engine->control(task, reason, resume) : FETCHKIT_CLOSED;
}

fetchkit_status readSnapshot(const std::shared_ptr<Task>& task, fetchkit_task_snapshot* output, std::uint32_t timeout) {
    if (!output || output->struct_size < sizeof(*output) || timeout > 60000) return FETCHKIT_INVALID_ARGUMENT;
    const auto owner = task->owner.lock();
    if (timeout && owner && owner->onWorker()) return FETCHKIT_BUSY;
    std::unique_lock<std::mutex> lock(task->mutex);
    bool ready = true;
    if (timeout) ready = task->changed.wait_for(lock, std::chrono::milliseconds(timeout), [&] { return stoppedState(task->snapshot.state); });
    *output = task->snapshot;
    return ready ? FETCHKIT_OK : FETCHKIT_TIMEOUT;
}

}

extern "C" {

void fetchkit_engine_config_init(fetchkit_engine_config* config) noexcept {
    if (!config) return;
    *config = {};
    config->struct_size = sizeof(*config);
    config->connect_timeout_ms = 5000;
    config->read_timeout_ms = 15000;
    config->provider_timeout_ms = 10000;
    config->max_redirects = 5;
    config->max_requests = 4096;
}

void fetchkit_download_request_init(fetchkit_download_request* request) noexcept {
    if (!request) return;
    *request = {};
    request->struct_size = sizeof(*request);
}

const char* fetchkit_status_name(fetchkit_status status) noexcept {
    static const char* names[] = {"Ok", "InvalidArgument", "NotSupported", "Closed", "Busy", "Network", "Timeout",
        "HttpStatus", "AuthRequired", "ProtocolMismatch", "IntegrityMismatch", "DiskFull", "PermissionDenied", "IoError",
        "Cancelled", "QuotaExceeded", "OutOfMemory", "InternalError", "CertificateError"};
    return status < sizeof(names) / sizeof(names[0]) ? names[status] : "UnknownError";
}

fetchkit_status fetchkit_engine_create(const fetchkit_engine_config* input, fetchkit_engine* output) noexcept {
    if (output) *output = 0;
    return boundary([&]() -> fetchkit_status {
        if (!input || !output || input->struct_size < sizeof(*input) || input->allow_http > 1 || input->reserved ||
            input->max_download_bytes == 0 || input->max_download_bytes > static_cast<std::uint64_t>(INT64_MAX) ||
            !input->connect_timeout_ms || !input->read_timeout_ms || !input->provider_timeout_ms ||
            input->connect_timeout_ms > 300000 || input->read_timeout_ms > 300000 || input->provider_timeout_ms > 60000 ||
            input->max_redirects > 32 || !input->max_requests || input->max_requests > 65536) return FETCHKIT_INVALID_ARGUMENT;
        EngineConfig config;
        config.root = text(input->storage_root, 3500, true);
        config.ca_bundle = text(input->ca_bundle, 3500);
        config.max_bytes = input->max_download_bytes;
        config.allow_http = input->allow_http != 0;
        config.connect_timeout_ms = input->connect_timeout_ms;
        config.read_timeout_ms = input->read_timeout_ms;
        config.provider_timeout_ms = input->provider_timeout_ms;
        config.max_redirects = input->max_redirects;
        config.max_requests = input->max_requests;
#ifdef FETCHKIT_WITH_CURL
        auto engine = std::make_shared<Engine>(config, std::make_shared<transport::CurlTransport>());
        const auto id = registry().allocate();
        { std::lock_guard<std::mutex> lock(registry().mutex); registry().engines.emplace(id, std::move(engine)); }
        *output = id;
        return FETCHKIT_OK;
#else
        return FETCHKIT_NOT_SUPPORTED;
#endif
    });
}

fetchkit_status fetchkit_engine_close(fetchkit_engine id) noexcept {
    return boundary([&]() -> fetchkit_status {
        auto engine = engineFor(id);
        const auto status = engine->close();
        if (status == FETCHKIT_OK) { std::lock_guard<std::mutex> lock(registry().mutex); registry().engines.erase(id); }
        return status;
    });
}

fetchkit_status fetchkit_enqueue(fetchkit_engine id, const fetchkit_download_request* input, fetchkit_task* output) noexcept {
    if (output) *output = 0;
    return boundary([&]() -> fetchkit_status {
        if (!input || !output || input->struct_size < sizeof(*input) || input->expected_length_known > 1 || input->reserved ||
            input->header_count > 64 || (input->header_count && !input->headers) ||
            input->expected_length > static_cast<std::uint64_t>(INT64_MAX)) return FETCHKIT_INVALID_ARGUMENT;
        auto engine = engineFor(id);
        DownloadRequest request;
        request.resource_namespace = text(input->resource_namespace, 256, true);
        request.resource_key = text(input->resource_key, 1024, true);
        request.version = text(input->version, 256, true);
        request.representation_key = text(input->representation_key, 1024);
        request.reference = text(input->address_reference, 2048);
        request.request.url = text(input->url, 16384, !input->provider);
        if (input->expected_length_known) request.expected_length = input->expected_length;
        for (std::uint32_t i = 0; i < input->header_count; ++i) {
            request.request.headers.push_back({text(input->headers[i].name, 256, true), text(input->headers[i].value, 8192)});
        }
        validateHeaders(request.request.headers);
        if (input->provider) {
            const auto provider = input->provider;
            void* const user = input->provider_user_data;
            request.provider = [provider, user](const ProviderContext& context, RequestData& data, transport::Cancellation& cancellation) {
                const std::string proposed_url = data.url;
                fetchkit_request_context public_context{sizeof(fetchkit_request_context), context.reason, context.task_id,
                    context.generation, context.http_status, 0, view(proposed_url), view(context.previous_origin), view(context.reference)};
                fetchkit_request_builder builder{&data, &cancellation, FETCHKIT_OK};
                fetchkit_status status = FETCHKIT_AUTH_REQUIRED;
                try { status = provider(user, &public_context, &builder); } catch (...) { throw Failure(FETCHKIT_AUTH_REQUIRED); }
                if (builder.error) throw Failure(builder.error);
                if (status != FETCHKIT_OK) throw Failure(status == FETCHKIT_CANCELLED ? FETCHKIT_CANCELLED : FETCHKIT_AUTH_REQUIRED);
            };
        }
        const auto task_id = registry().allocate();
        { std::lock_guard<std::mutex> lock(registry().mutex); registry().tasks.emplace(task_id, nullptr); }
        try {
            engine->enqueue(std::move(request), task_id, [&](const std::shared_ptr<Task>& task) {
                std::lock_guard<std::mutex> lock(registry().mutex); registry().tasks.at(task_id) = task;
            });
        } catch (...) {
            std::lock_guard<std::mutex> lock(registry().mutex); registry().tasks.erase(task_id); throw;
        }
        *output = task_id;
        return FETCHKIT_OK;
    });
}

fetchkit_status fetchkit_task_query(fetchkit_task task, fetchkit_task_snapshot* output) noexcept {
    return boundary([&] { return readSnapshot(taskFor(task), output, 0); });
}
fetchkit_status fetchkit_task_wait(fetchkit_task task, std::uint32_t timeout, fetchkit_task_snapshot* output) noexcept {
    return boundary([&] { return readSnapshot(taskFor(task), output, timeout); });
}
fetchkit_status fetchkit_task_pause(fetchkit_task task) noexcept { return boundary([&] { return control(task, StopReason::Pause); }); }
fetchkit_status fetchkit_task_resume(fetchkit_task task) noexcept { return boundary([&] { return control(task, StopReason::None, true); }); }
fetchkit_status fetchkit_task_cancel(fetchkit_task task) noexcept { return boundary([&] { return control(task, StopReason::Cancel); }); }

fetchkit_status fetchkit_task_release(fetchkit_task id) noexcept {
    return boundary([&]() -> fetchkit_status {
        auto task = taskFor(id);
        auto engine = task->owner.lock();
        if (engine && engine->onWorker()) return FETCHKIT_BUSY;
        {
            std::lock_guard<std::mutex> lock(task->mutex); task->released = true;
        }
        if (engine) engine->control(task, StopReason::Pause);
        {
            std::unique_lock<std::mutex> lock(task->mutex);
            task->changed.wait(lock, [&] { return stoppedState(task->snapshot.state); });
        }
        { std::lock_guard<std::mutex> lock(registry().mutex); registry().tasks.erase(id); }
        return FETCHKIT_OK;
    });
}

fetchkit_status fetchkit_request_set_url(fetchkit_request_builder* builder, fetchkit_string url) noexcept {
    const auto status = boundary([&]() -> fetchkit_status {
        if (!builder) return FETCHKIT_INVALID_ARGUMENT;
        builder->data->url = text(url, 16384, true);
        return FETCHKIT_OK;
    });
    if (builder && status) builder->error = status;
    return status;
}

fetchkit_status fetchkit_request_set_header(fetchkit_request_builder* builder, fetchkit_string name, fetchkit_string value) noexcept {
    const auto status = boundary([&]() -> fetchkit_status {
        if (!builder) return FETCHKIT_INVALID_ARGUMENT;
        transport::Header header{text(name, 256, true), text(value, 8192)};
        if (!transport::validHeader(header) || protectedHeader(header.name)) return FETCHKIT_INVALID_ARGUMENT;
        const auto lower = transport::lowerAscii(header.name);
        auto& headers = builder->data->headers;
        const auto existing = std::find_if(headers.begin(), headers.end(), [&](const transport::Header& value) {
            return transport::lowerAscii(value.name) == lower;
        });
        if (existing == headers.end() && headers.size() >= 64) return FETCHKIT_INVALID_ARGUMENT;
        auto& explicit_names = builder->data->explicitly_set_headers;
        if (std::find(explicit_names.begin(), explicit_names.end(), lower) == explicit_names.end()) explicit_names.push_back(lower);
        if (existing != headers.end()) *existing = std::move(header); else headers.push_back(std::move(header));
        return FETCHKIT_OK;
    });
    if (builder && status) builder->error = status;
    return status;
}

std::uint32_t fetchkit_request_is_cancelled(const fetchkit_request_builder* builder) noexcept {
    return !builder || builder->cancellation->isCancelled();
}

}
