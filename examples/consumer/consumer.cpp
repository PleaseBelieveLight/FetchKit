#include <fetchkit/fetchkit.hpp>
#include <array>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    try {
        const std::string root = argv[1], url = argv[2], digest = argv[3];
        const std::string name = "consumer", key = "cpp", version = "1";
        fetchkit_engine_config config{};
        fetchkit_engine_config_init(&config);
        config.storage_root = fetchkit::stringView(root);
        config.max_download_bytes = 1048576;
        config.storage_capacity_bytes = 4194304;
        config.staging_retention_ms = 86400000;
        config.allow_http = 1;
        fetchkit::Engine engine(config);
        fetchkit_download_request request{};
        fetchkit_download_request_init(&request);
        request.resource_namespace = fetchkit::stringView(name);
        request.resource_key = fetchkit::stringView(key);
        request.version = fetchkit::stringView(version);
        request.url = fetchkit::stringView(url);
        request.expected_sha256 = fetchkit::stringView(digest);
        request.require_digest = 1;
        auto task = engine.enqueue(request);
        const auto snapshot = task.wait(30000);
        if (snapshot.state != FETCHKIT_COMPLETED || snapshot.integrity != FETCHKIT_DIGEST_VERIFIED) return 1;
        auto lease = engine.acquire(snapshot.persistent_id);
        fetchkit::checkStatus(engine.close()); // Lease owns the independent read context.
        std::array<unsigned char, 65536> buffer{};
        std::uint64_t offset = 0;
        while (const auto count = lease.read(offset, buffer.data(), buffer.size())) {
            for (std::uint32_t i = 0; i < count; ++i) {
                if (buffer[i] != static_cast<unsigned char>((offset + i) % 251)) return 1;
            }
            offset += count;
        }
        if (offset != snapshot.received_bytes || offset != 262144) return 1;
        std::cout << "{\"passed\":true,\"language\":\"C++\",\"bytes\":" << offset << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
