#include "fetchkit/fetchkit.h"
#include <stdio.h>
#include <string.h>

#define CHECK(expression) do { if (!(expression)) { fprintf(stderr, "C API check failed at line %d\n", __LINE__); return 1; } } while (0)
static fetchkit_string view(const char* text) { fetchkit_string result = {text, (uint64_t)strlen(text)}; return result; }
static fetchkit_status no_network_provider(void* user, const fetchkit_request_context* context, fetchkit_request_builder* builder) {
    (void)user; (void)context; (void)builder;
    return FETCHKIT_AUTH_REQUIRED;
}

int main(int argc, char** argv) {
    fetchkit_engine_config config;
    fetchkit_engine engine = 42, second = 0;
    fetchkit_task_snapshot snapshot = {0};
    fetchkit_download_request request;
    fetchkit_task task = 0;
    CHECK(argc == 2);
    fetchkit_engine_config_init(&config);
    CHECK(fetchkit_engine_create(&config, &engine) == FETCHKIT_INVALID_ARGUMENT && engine == 0);
    config.storage_root = view(argv[1]);
    config.max_download_bytes = 1048576;
    config.storage_capacity_bytes=16777216;config.staging_retention_ms=86400000;
    {
        fetchkit_engine_config invalid=config;
        invalid.storage_capacity_bytes=0;CHECK(fetchkit_engine_create(&invalid,&engine)==FETCHKIT_INVALID_ARGUMENT);
        invalid=config;invalid.staging_retention_ms=0;CHECK(fetchkit_engine_create(&invalid,&engine)==FETCHKIT_INVALID_ARGUMENT);
        invalid=config;invalid.max_active_downloads=17;CHECK(fetchkit_engine_create(&invalid,&engine)==FETCHKIT_INVALID_ARGUMENT);
        invalid=config;invalid.max_retries=17;CHECK(fetchkit_engine_create(&invalid,&engine)==FETCHKIT_INVALID_ARGUMENT);
    }
#ifdef FETCHKIT_TEST_CURL
    CHECK(fetchkit_engine_create(&config, &engine) == FETCHKIT_OK && engine != 0);
    CHECK(fetchkit_engine_create(&config, &second) == FETCHKIT_BUSY && second == 0);
    fetchkit_download_request_init(&request);
    request.resource_namespace = view("test"); request.resource_key = view("resource"); request.version = view("1");
    request.url = view("https://user:secret@localhost/file");
    CHECK(fetchkit_enqueue(engine, &request, &task) == FETCHKIT_INVALID_ARGUMENT && task == 0);
    request.url = view("http://localhost/file");
    CHECK(fetchkit_enqueue(engine, &request, &task) == FETCHKIT_INVALID_ARGUMENT);
    request.url = view("https://localhost/file"); request.require_digest = 1;
    CHECK(fetchkit_enqueue(engine, &request, &task) == FETCHKIT_INVALID_ARGUMENT);
    request.expected_sha256 = view("bad");
    CHECK(fetchkit_enqueue(engine, &request, &task) == FETCHKIT_INVALID_ARGUMENT);
    request.expected_sha256=view("");request.require_digest=0;request.pin_on_create=2;
    CHECK(fetchkit_enqueue(engine,&request,&task)==FETCHKIT_INVALID_ARGUMENT);
    {
        uint32_t count = 9;
        fetchkit_lease lease = 9;
        fetchkit_task_info item = {0}; item.struct_size = sizeof(item);
        CHECK(fetchkit_list_tasks(engine, 0, &item, 101, &count) == FETCHKIT_INVALID_ARGUMENT && count == 0);
        CHECK(fetchkit_resource_acquire(engine, view("../escape"), &lease) == FETCHKIT_INVALID_ARGUMENT && lease == 0);
        CHECK(fetchkit_resource_read(UINT64_MAX, 0, NULL, 1, &count) == FETCHKIT_INVALID_ARGUMENT && count == 0);
        CHECK(fetchkit_resource_read(UINT64_MAX, 0, NULL, 0, &count) == FETCHKIT_CLOSED);
        CHECK(fetchkit_resource_release(UINT64_MAX) == FETCHKIT_CLOSED);
    }
    {
        uint32_t count = 0;
        fetchkit_task_info item = {0}; item.struct_size = sizeof(item);
        fetchkit_task_snapshot current = {0}; current.struct_size = sizeof(current);
        request.pin_on_create = 0; request.url = view(""); request.provider = no_network_provider;
        CHECK(fetchkit_enqueue(engine, &request, &task) == FETCHKIT_OK);
        CHECK(fetchkit_task_wait(task, 10000, &current) == FETCHKIT_OK && current.state == FETCHKIT_FAILED);
        CHECK(fetchkit_resource_pin(engine, view(current.persistent_id), 1) == FETCHKIT_OK);
        CHECK(fetchkit_resource_set_expiry(engine, view(current.persistent_id), 12345) == FETCHKIT_OK);
        CHECK(fetchkit_list_tasks(engine, 0, &item, 1, &count) == FETCHKIT_OK && count == 1);
        CHECK(fetchkit_task_query(task, &current) == FETCHKIT_OK);
        CHECK(item.snapshot.pinned == 1 && item.snapshot.expires_at_ms == 12345);
        CHECK(item.snapshot.pinned == current.pinned && item.snapshot.expires_at_ms == current.expires_at_ms);
        CHECK(fetchkit_resource_pin(engine, view(current.persistent_id), 0) == FETCHKIT_OK);
        CHECK(fetchkit_resource_set_expiry(engine, view(current.persistent_id), 0) == FETCHKIT_OK);
        CHECK(fetchkit_list_tasks(engine, 0, &item, 1, &count) == FETCHKIT_OK && count == 1);
        CHECK(item.snapshot.pinned == 0 && item.snapshot.expires_at_ms == 0);
        CHECK(fetchkit_task_release(task) == FETCHKIT_OK);
        CHECK(fetchkit_resource_remove(engine, view(current.persistent_id)) == FETCHKIT_OK);
    }
    CHECK(fetchkit_engine_close(engine) == FETCHKIT_OK);
    CHECK(fetchkit_engine_close(engine) == FETCHKIT_CLOSED);
    CHECK(fetchkit_engine_create(&config, &engine) == FETCHKIT_OK);
    CHECK(fetchkit_engine_close(engine) == FETCHKIT_OK);
#else
    CHECK(fetchkit_engine_create(&config, &engine) == FETCHKIT_NOT_SUPPORTED && engine == 0);
#endif
    snapshot.struct_size = sizeof(snapshot);
    CHECK(fetchkit_task_query(UINT64_MAX, &snapshot) == FETCHKIT_CLOSED);
    CHECK(fetchkit_task_release(UINT64_MAX) == FETCHKIT_CLOSED);
    CHECK(fetchkit_request_set_url(NULL, view("https://localhost/")) == FETCHKIT_INVALID_ARGUMENT);
    return 0;
}
