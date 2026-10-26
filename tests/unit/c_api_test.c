#include "fetchkit/fetchkit.h"
#include <stdio.h>
#include <string.h>

#define CHECK(expression) do { if (!(expression)) { fprintf(stderr, "C API check failed at line %d\n", __LINE__); return 1; } } while (0)
static fetchkit_string view(const char* text) { fetchkit_string result = {text, (uint64_t)strlen(text)}; return result; }

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
#ifdef FETCHKIT_TEST_CURL
    CHECK(fetchkit_engine_create(&config, &engine) == FETCHKIT_OK && engine != 0);
    CHECK(fetchkit_engine_create(&config, &second) == FETCHKIT_BUSY && second == 0);
    fetchkit_download_request_init(&request);
    request.resource_namespace = view("test"); request.resource_key = view("resource"); request.version = view("1");
    request.url = view("https://user:secret@localhost/file");
    CHECK(fetchkit_enqueue(engine, &request, &task) == FETCHKIT_INVALID_ARGUMENT && task == 0);
    request.url = view("http://localhost/file");
    CHECK(fetchkit_enqueue(engine, &request, &task) == FETCHKIT_INVALID_ARGUMENT);
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
