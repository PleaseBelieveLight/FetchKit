#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#define FETCHKIT_NOEXCEPT noexcept
#else
#define FETCHKIT_NOEXCEPT
#endif

/* M3 alpha ABI；升级 SDK 后须重新编译客户端，尚未冻结稳定 ABI。 */
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
    FETCHKIT_NOT_FOUND, FETCHKIT_UNSUPPORTED_METADATA, FETCHKIT_SUBSCRIPTION_OVERFLOW
};
enum {
    FETCHKIT_QUEUED = 0, FETCHKIT_RUNNING, FETCHKIT_PAUSED, FETCHKIT_TRANSFER_COMPLETE,
    FETCHKIT_FAILED, FETCHKIT_TASK_CANCELLED, FETCHKIT_PAUSING,
    FETCHKIT_VERIFYING, FETCHKIT_PUBLISHING, FETCHKIT_COMPLETED, FETCHKIT_RETRY_WAITING,
    FETCHKIT_REMOVED
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
/* 在 transport worker 上执行，不持 SDK 核心锁；借用参数仅在本次回调内有效，setter 立即复制。
 * provider 须协作检查取消，不能阻塞等待 SDK 工作线程；宿主须保持 user_data 有效，
 * 直到对应订阅 release 或 engine close 完成。 */
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
    uint64_t storage_capacity_bytes; /* Required positive capacity including metadata/reservations. */
    uint64_t staging_retention_ms; /* Required positive age for inactive recorded staging cleanup. */
    uint32_t max_active_downloads; /* Default 3, hard maximum 16. */
    uint32_t max_queued_tasks; /* Default/hard maximum 1024. */
    uint32_t max_retries; /* Extra attempts, default 3, hard maximum 16. */
    uint32_t retry_base_delay_ms; /* Default 1000. */
    uint32_t retry_max_delay_ms; /* Default 30000; does not shorten Retry-After. */
    uint32_t reserved3;
    uint64_t retry_random_seed; /* Zero selects an unpredictable seed. */
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
    uint64_t expires_at_ms; /* UTC Unix milliseconds; zero means no expiration. */
    uint32_t pin_on_create; /* Persist retention atomically with task admission. */
    uint32_t reserved3;
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
    /* 暂存路径仅供诊断；不能当作已发布资源读取，也不应直接写入日志。 */
    char staging_path[4096];
    uint64_t committed_offset;
    uint32_t integrity;
    uint32_t reserved2;
    char persistent_id[65]; /* Stable SHA-256 of length-bounded identity, unrelated to URL. */
    uint32_t retry_count;
    uint32_t subscriber_count;
    uint64_t next_retry_at_ms;
    uint64_t expires_at_ms;
    uint32_t available;
    uint32_t pinned;
} fetchkit_task_snapshot;

enum { FETCHKIT_EVENT_STATE = 0, FETCHKIT_EVENT_PROGRESS, FETCHKIT_EVENT_SUBSCRIPTION_ERROR };
typedef struct fetchkit_event {
    uint32_t struct_size;
    uint32_t kind;
    uint64_t task_id; /* Subscription token. */
    uint64_t generation;
    uint32_t state;
    fetchkit_status error;
    uint64_t received_bytes;
    uint64_t committed_offset;
    uint64_t total_bytes;
    uint32_t total_known;
    uint32_t integrity;
    uint32_t http_status;
    uint32_t retry_count;
    uint64_t next_retry_at_ms;
} fetchkit_event;
typedef void (*fetchkit_event_callback)(void* user_data, const fetchkit_event* event);

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
    uint32_t orphan_files; /* 孤立文件计入配额并保留；不根据文件名推断归属后自动删除。 */
    uint32_t reserved;
    uint64_t capacity_bytes;
    uint64_t used_bytes;
    uint64_t reserved_bytes;
    uint64_t charged_bytes;
    uint64_t metadata_budget_bytes;
} fetchkit_storage_stats;

enum { FETCHKIT_LOG_OFF = 0, FETCHKIT_LOG_ERROR, FETCHKIT_LOG_WARN, FETCHKIT_LOG_DEBUG };
/* 当前库实例内的进程级日志开关，线程安全，默认 Off；级别包含更严重的消息。
 * Android 输出到 Logcat（tag: FetchKit），主机输出到 stderr；不影响错误返回或事件。
 * 非法级别返回 InvalidArgument，保留原设置。 */
fetchkit_status fetchkit_set_log_level(uint32_t level) FETCHKIT_NOEXCEPT;

void fetchkit_engine_config_init(fetchkit_engine_config* config) FETCHKIT_NOEXCEPT;
void fetchkit_download_request_init(fetchkit_download_request* request) FETCHKIT_NOEXCEPT;
const char* fetchkit_status_name(fetchkit_status status) FETCHKIT_NOEXCEPT;
fetchkit_status fetchkit_engine_create(const fetchkit_engine_config* config, fetchkit_engine* engine) FETCHKIT_NOEXCEPT;
/* close/release 等待 SDK 自有 I/O 与 provider 静止；在 provider 线程调用返回 Busy，避免自等待。
 * 旧 token 安全失败；关闭保留可恢复记录，已有 lease 继续持有目录锁。 */
fetchkit_status fetchkit_engine_close(fetchkit_engine engine) FETCHKIT_NOEXCEPT;
/* 入队复制请求所需字段并创建独立订阅；同身份共享 writer，约束冲突时失败。 */
fetchkit_status fetchkit_enqueue(fetchkit_engine engine, const fetchkit_download_request* request, fetchkit_task* task) FETCHKIT_NOEXCEPT;
/* attach 重新绑定凭证/约束但不启动恢复任务；同身份 enqueue 则显式给予执行许可。 */
fetchkit_status fetchkit_attach(fetchkit_engine engine, const fetchkit_download_request* request, fetchkit_task* task) FETCHKIT_NOEXCEPT;
/* 按稳定 ID 顺序分页，capacity 为 1～100；打开引擎时已同步完成扫描修复。 */
fetchkit_status fetchkit_list_tasks(fetchkit_engine engine, uint32_t offset, fetchkit_task_info* items, uint32_t capacity, uint32_t* count) FETCHKIT_NOEXCEPT;
fetchkit_status fetchkit_engine_storage_stats(fetchkit_engine engine, fetchkit_storage_stats* stats) FETCHKIT_NOEXCEPT;
/* 仅允许获取未过期且一致的 Completed 对象，不暴露暂存路径；成功返回的 lease 须显式释放。
 * engine 关闭后 lease 仍可读并保持目录锁；read 使用字节偏移，EOF 的 read_count 为 0。 */
fetchkit_status fetchkit_resource_acquire(fetchkit_engine engine, fetchkit_string persistent_id, fetchkit_lease* lease) FETCHKIT_NOEXCEPT;
fetchkit_status fetchkit_resource_read(fetchkit_lease lease, uint64_t offset, void* buffer, uint32_t capacity, uint32_t* read_count) FETCHKIT_NOEXCEPT;
fetchkit_status fetchkit_resource_release(fetchkit_lease lease) FETCHKIT_NOEXCEPT;
fetchkit_status fetchkit_resource_pin(fetchkit_engine engine, fetchkit_string persistent_id, uint32_t pinned) FETCHKIT_NOEXCEPT;
fetchkit_status fetchkit_resource_set_expiry(fetchkit_engine engine, fetchkit_string persistent_id, uint64_t expires_at_ms) FETCHKIT_NOEXCEPT;
fetchkit_status fetchkit_resource_remove(fetchkit_engine engine, fetchkit_string persistent_id) FETCHKIT_NOEXCEPT;
fetchkit_status fetchkit_storage_trim(fetchkit_engine engine, uint64_t target_bytes) FETCHKIT_NOEXCEPT;
fetchkit_status fetchkit_storage_collect_staging(fetchkit_engine engine) FETCHKIT_NOEXCEPT;
fetchkit_status fetchkit_task_query(fetchkit_task task, fetchkit_task_snapshot* snapshot) FETCHKIT_NOEXCEPT;
fetchkit_status fetchkit_task_wait(fetchkit_task task, uint32_t timeout_ms, fetchkit_task_snapshot* snapshot) FETCHKIT_NOEXCEPT;
/* pause/resume 影响共享任务；控制返回 Ok 仅表示请求已接受，须查询/等待 Paused 才确认检查点成功。 */
fetchkit_status fetchkit_task_pause(fetchkit_task task) FETCHKIT_NOEXCEPT;
fetchkit_status fetchkit_task_resume(fetchkit_task task) FETCHKIT_NOEXCEPT;
/* cancel 只撤销本订阅，最后一个显式取消才清理共享未完成数据；已完成对象保留。 */
fetchkit_status fetchkit_task_cancel(fetchkit_task task) FETCHKIT_NOEXCEPT;
/* release 静默解除订阅；最后一个释放时停止 I/O 并尽力保留检查点，区别于显式 cancel。 */
fetchkit_status fetchkit_task_release(fetchkit_task task) FETCHKIT_NOEXCEPT;
/* 由宿主在选定 executor 上同步调用，每订阅串行，回调不持 SDK 锁；max_events 为 1～64。
 * 队列最多 64 项，进度可合并；状态溢出只终止对应订阅。event 仅在回调内借用，需要留存则复制。 */
fetchkit_status fetchkit_task_dispatch(fetchkit_task task, fetchkit_event_callback callback, void* user_data, uint32_t max_events, uint32_t* dispatched) FETCHKIT_NOEXCEPT;
fetchkit_status fetchkit_request_set_url(fetchkit_request_builder* builder, fetchkit_string url) FETCHKIT_NOEXCEPT;
fetchkit_status fetchkit_request_set_header(fetchkit_request_builder* builder, fetchkit_string name, fetchkit_string value) FETCHKIT_NOEXCEPT;
uint32_t fetchkit_request_is_cancelled(const fetchkit_request_builder* builder) FETCHKIT_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#undef FETCHKIT_NOEXCEPT
