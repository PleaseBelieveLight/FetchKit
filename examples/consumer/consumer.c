#include <fetchkit/fetchkit.h>
#include <stdio.h>
#include <string.h>

static fetchkit_string view(const char* text) {
    fetchkit_string result = {text, (uint64_t)strlen(text)};
    return result;
}

/* argv: storage root, URL, trusted SHA-256. Only installed public headers are used. */
int main(int argc, char** argv) {
    fetchkit_engine engine = 0;
    fetchkit_task task = 0;
    fetchkit_lease lease = 0;
    fetchkit_engine_config config;
    fetchkit_download_request request;
    fetchkit_task_snapshot snapshot = {0};
    uint64_t offset = 0;
    unsigned char buffer[65536];
    int result = 1;
    if (argc != 4) return 2;
    fetchkit_engine_config_init(&config);
    config.storage_root = view(argv[1]);
    config.max_download_bytes = 1048576;
    config.storage_capacity_bytes = 4194304;
    config.staging_retention_ms = 86400000;
    config.allow_http = 1; /* Local test server only; HTTPS remains the default. */
    if (fetchkit_engine_create(&config, &engine) != FETCHKIT_OK) goto cleanup;
    fetchkit_download_request_init(&request);
    request.resource_namespace = view("consumer");
    request.resource_key = view("c");
    request.version = view("1");
    request.url = view(argv[2]);
    request.expected_sha256 = view(argv[3]);
    request.require_digest = 1;
    if (fetchkit_enqueue(engine, &request, &task) != FETCHKIT_OK) goto cleanup;
    snapshot.struct_size = sizeof(snapshot);
    if (fetchkit_task_wait(task, 30000, &snapshot) != FETCHKIT_OK ||
        snapshot.state != FETCHKIT_COMPLETED || snapshot.integrity != FETCHKIT_DIGEST_VERIFIED) goto cleanup;
    if (fetchkit_resource_acquire(engine, view(snapshot.persistent_id), &lease) != FETCHKIT_OK) goto cleanup;
    for (;;) {
        uint32_t count = 0;
        uint32_t i;
        if (fetchkit_resource_read(lease, offset, buffer, sizeof(buffer), &count) != FETCHKIT_OK) goto cleanup;
        if (!count) break;
        for (i = 0; i < count; ++i) {
            if (buffer[i] != (unsigned char)((offset + i) % 251)) goto cleanup;
        }
        offset += count;
    }
    if (offset != snapshot.received_bytes || offset != 262144) goto cleanup;
    printf("{\"passed\":true,\"language\":\"C\",\"bytes\":%llu}\n", (unsigned long long)offset);
    result = 0;
cleanup:
    if (lease) fetchkit_resource_release(lease);
    if (task) fetchkit_task_release(task);
    if (engine && fetchkit_engine_close(engine) != FETCHKIT_OK) result = 1;
    return result;
}
