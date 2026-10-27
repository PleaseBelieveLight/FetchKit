#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#define FETCHKIT_NOEXCEPT noexcept
#else
#define FETCHKIT_NOEXCEPT
#endif

/* M2 alpha ABI. Recompile clients when updating; not a stable release ABI. */
typedef uint64_t fetchkit_engine;
typedef uint64_t fetchkit_task;
typedef uint64_t fetchkit_lease;
typedef uint32_t fetchkit_status;
enum {
    FETCHKIT_OK = 0, FETCHKIT_INVALID_ARGUMENT, FETCHKIT_NOT_SUPPORTED, FETCHKIT_CLOSED,
    FETCHKIT_BUSY, FETCHKIT_NETWORK, FETCHKIT_TIMEOUT, FETCHKIT_HTTP_STATUS,
    FETCHKIT_AUTH_REQUIRED, FETCHKIT_PROTOCOL_MISMATCH, FETCHKIT_INTEGRITY_MISMATCH,
    FETCHKIT_DISK_FULL, FETCHKIT_PERMISSION_DENIED, FETCHKIT_IO_ERROR, FETCHKIT_CANCELLED,
    FETCHKIT_QUOTA_EXCEEDED, FETCHKIT_OUT_OF_MEMORY, FETCHKIT_INTERNAL_ERROR, FETCHKIT_CERTIFICATE_ERROR,
    FETCHKIT_NOT_FOUND, FETCHKIT_UNSUPPORTED_METADATA
};
enum {
    FETCHKIT_QUEUED = 0, FETCHKIT_RUNNING, FETCHKIT_PAUSED, FETCHKIT_TRANSFER_COMPLETE,
    FETCHKIT_FAILED, FETCHKIT_TASK_CANCELLED, FETCHKIT_PAUSING,
    FETCHKIT_VERIFYING, FETCHKIT_PUBLISHING, FETCHKIT_COMPLETED
};
enum { FETCHKIT_INTEGRITY_NONE = 0, FETCHKIT_TRANSPORT_COMPLETE, FETCHKIT_DIGEST_VERIFIED };
enum { FETCHKIT_REQUEST_INITIAL = 0, FETCHKIT_REQUEST_REDIRECT, FETCHKIT_REQUEST_AUTH_REFRESH };

typedef struct fetchkit_string {
    const char* data;
    uint64_t size;
} fetchkit_string;

typedef struct fetchkit_header {
    fetchkit_string name;
    fetchkit_string value;
} fetchkit_header;

typedef struct fetchkit_request_context {
    uint32_t struct_size;
    uint32_t reason;
    uint64_t task_id;
    uint64_t generation;
    uint32_t http_status;
    uint32_t reserved;
    fetchkit_string proposed_url;
    fetchkit_string previous_origin;
    fetchkit_string address_reference;
} fetchkit_request_context;

typedef struct fetchkit_request_builder fetchkit_request_builder;
/* Runs on the transport worker without SDK locks. Borrowed arguments expire on
 * return. Setters copy immediately. The provider must cooperate with cancellation;
 * user_data remains alive until task release or engine close has completed. */
typedef fetchkit_status (*fetchkit_request_provider)(
    void* user_data, const fetchkit_request_context* context, fetchkit_request_builder* builder);

typedef struct fetchkit_engine_config {
    uint32_t struct_size;
    uint32_t allow_http;
    fetchkit_string storage_root;
    uint64_t max_download_bytes; /* Required positive per-task bound, <= INT64_MAX. */
    uint32_t connect_timeout_ms;
    uint32_t read_timeout_ms;
    uint32_t provider_timeout_ms;
    uint32_t max_redirects;
    uint32_t max_requests;
    uint32_t reserved;
    fetchkit_string ca_bundle; /* Optional trusted CA bundle, never disables TLS verification. */
    uint64_t checkpoint_bytes; /* Positive; default 4 MiB. */
    uint32_t checkpoint_interval_ms; /* Positive; default 1000 ms. Checked on incoming data. */
    uint32_t reserved2;
} fetchkit_engine_config;

typedef struct fetchkit_download_request {
    uint32_t struct_size;
    uint32_t expected_length_known;
    fetchkit_string resource_namespace;
    fetchkit_string resource_key;
    fetchkit_string version;
    fetchkit_string representation_key;
    fetchkit_string url;
    fetchkit_string address_reference;
    uint64_t expected_length;
    const fetchkit_header* headers;
    uint32_t header_count;
    uint32_t reserved;
    fetchkit_request_provider provider;
    void* provider_user_data;
    fetchkit_string expected_sha256; /* Empty or 64 hexadecimal characters, trusted by the host. */
    uint32_t require_digest;
    uint32_t reserved2;
} fetchkit_download_request;

typedef struct fetchkit_task_snapshot {
    uint32_t struct_size;
    uint32_t state;
    uint64_t task_id;
    uint64_t generation;
    uint64_t received_bytes;
    uint64_t total_bytes;
    uint32_t total_known;
    uint32_t http_status;
    fetchkit_status error;
    int32_t platform_code;
    uint32_t can_resume;
    uint32_t request_count;
    /* Diagnostic staging location only. This is NOT a published/acquired resource. */
    char staging_path[4096];
    uint64_t committed_offset;
    uint32_t integrity;
    uint32_t reserved2;
    char persistent_id[65]; /* Stable SHA-256 of length-bounded identity, unrelated to URL. */
} fetchkit_task_snapshot;

typedef struct fetchkit_task_info {
    uint32_t struct_size;
    uint32_t reserved;
    fetchkit_task_snapshot snapshot; /* task_id is zero until attached. */
    char resource_namespace[257];
    char resource_key[1025];
    char version[257];
    char representation_key[1025];
    char address_reference[2049];
} fetchkit_task_info;

typedef struct fetchkit_storage_stats {
    uint32_t struct_size;
    uint32_t record_count;
    uint32_t quarantined_records;
    uint32_t unsupported_records;
    uint32_t orphan_files; /* Counted, retained; automatic reclamation belongs to M3. */
    uint32_t reserved;
} fetchkit_storage_stats;

void fetchkit_engine_config_init(fetchkit_engine_config* config) FETCHKIT_NOEXCEPT;
void fetchkit_download_request_init(fetchkit_download_request* request) FETCHKIT_NOEXCEPT;
const char* fetchkit_status_name(fetchkit_status status) FETCHKIT_NOEXCEPT;
fetchkit_status fetchkit_engine_create(const fetchkit_engine_config* config, fetchkit_engine* engine) FETCHKIT_NOEXCEPT;
/* Close/release wait for SDK-owned I/O and providers. On the provider thread they
 * return Busy instead of joining themselves. Repeated/stale tokens fail safely. */
fetchkit_status fetchkit_engine_close(fetchkit_engine engine) FETCHKIT_NOEXCEPT;
fetchkit_status fetchkit_enqueue(fetchkit_engine engine, const fetchkit_download_request* request, fetchkit_task* task) FETCHKIT_NOEXCEPT;
/* Attach rebinds credentials and constraints, leaves a recovered task paused.
 * Re-enqueueing the same identity also rebinds, but explicitly starts execution. */
fetchkit_status fetchkit_attach(fetchkit_engine engine, const fetchkit_download_request* request, fetchkit_task* task) FETCHKIT_NOEXCEPT;
/* Stable ID order, maximum 100 items. Open-time repairs run synchronously. */
fetchkit_status fetchkit_list_tasks(fetchkit_engine engine, uint32_t offset, fetchkit_task_info* items, uint32_t capacity, uint32_t* count) FETCHKIT_NOEXCEPT;
fetchkit_status fetchkit_engine_storage_stats(fetchkit_engine engine, fetchkit_storage_stats* stats) FETCHKIT_NOEXCEPT;
/* Only consistent Completed objects can be acquired. No staging path is exposed.
 * A lease keeps the directory lock alive after engine close. Reads use offsets. */
fetchkit_status fetchkit_resource_acquire(fetchkit_engine engine, fetchkit_string persistent_id, fetchkit_lease* lease) FETCHKIT_NOEXCEPT;
fetchkit_status fetchkit_resource_read(fetchkit_lease lease, uint64_t offset, void* buffer, uint32_t capacity, uint32_t* read_count) FETCHKIT_NOEXCEPT;
fetchkit_status fetchkit_resource_release(fetchkit_lease lease) FETCHKIT_NOEXCEPT;
fetchkit_status fetchkit_task_query(fetchkit_task task, fetchkit_task_snapshot* snapshot) FETCHKIT_NOEXCEPT;
fetchkit_status fetchkit_task_wait(fetchkit_task task, uint32_t timeout_ms, fetchkit_task_snapshot* snapshot) FETCHKIT_NOEXCEPT;
fetchkit_status fetchkit_task_pause(fetchkit_task task) FETCHKIT_NOEXCEPT;
fetchkit_status fetchkit_task_resume(fetchkit_task task) FETCHKIT_NOEXCEPT;
fetchkit_status fetchkit_task_cancel(fetchkit_task task) FETCHKIT_NOEXCEPT;
fetchkit_status fetchkit_task_release(fetchkit_task task) FETCHKIT_NOEXCEPT;
fetchkit_status fetchkit_request_set_url(fetchkit_request_builder* builder, fetchkit_string url) FETCHKIT_NOEXCEPT;
fetchkit_status fetchkit_request_set_header(fetchkit_request_builder* builder, fetchkit_string name, fetchkit_string value) FETCHKIT_NOEXCEPT;
uint32_t fetchkit_request_is_cancelled(const fetchkit_request_builder* builder) FETCHKIT_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#undef FETCHKIT_NOEXCEPT
