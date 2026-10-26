#include "fetchkit/version.h"
#include "fetchkit/fetchkit.hpp"
#include "snapshot_json.h"

#include <charconv>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }

std::uint64_t number(const std::string& value) {
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) throw std::runtime_error("InvalidArgument");
    return result;
}

bool stopped(const fetchkit_task_snapshot& snapshot) {
    return snapshot.state == FETCHKIT_PAUSED || snapshot.state == FETCHKIT_TRANSFER_COMPLETE ||
        snapshot.state == FETCHKIT_FAILED || snapshot.state == FETCHKIT_TASK_CANCELLED;
}

int exitCode(const fetchkit_task_snapshot& snapshot) {
    if (snapshot.state == FETCHKIT_TRANSFER_COMPLETE) return 0;
    switch (snapshot.error) {
        case FETCHKIT_NETWORK: case FETCHKIT_TIMEOUT: case FETCHKIT_CERTIFICATE_ERROR: return 3;
        case FETCHKIT_HTTP_STATUS: case FETCHKIT_AUTH_REQUIRED: return 4;
        case FETCHKIT_PROTOCOL_MISMATCH: case FETCHKIT_INTEGRITY_MISMATCH: return 5;
        case FETCHKIT_IO_ERROR: case FETCHKIT_DISK_FULL: case FETCHKIT_PERMISSION_DENIED: case FETCHKIT_QUOTA_EXCEEDED: return 6;
        case FETCHKIT_CANCELLED: return 7;
        default: return 8;
    }
}

fetchkit_download_request requestFor(const std::string& url, const std::string& key) {
    fetchkit_download_request request{};
    fetchkit_download_request_init(&request);
    static const std::string resource_namespace = "cli", version = "1";
    request.resource_namespace = fetchkit::stringView(resource_namespace);
    request.resource_key = fetchkit::stringView(key);
    request.version = fetchkit::stringView(version);
    request.url = fetchkit::stringView(url);
    return request;
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--version") { std::cout << fetchkit::version() << '\n'; return 0; }
    if (argc < 2 || (std::string(argv[1]) != "download" && std::string(argv[1]) != "shell")) {
        std::cerr << "Usage: fetchkit_cli --version | download URL --root DIR [--allow-http] [--expected-length N]\n"
            "       fetchkit_cli shell --root DIR [--allow-http]\n"
            "M1 stages .part files only; no published resources or cross-process resume.\n";
        return argc == 1 ? 0 : 2;
    }
    try {
        const bool shell = std::string(argv[1]) == "shell";
        std::string url, root, ca_bundle, key = "resource";
        std::uint64_t pause_after = 0;
        fetchkit_engine_config config{};
        fetchkit_engine_config_init(&config);
        config.max_download_bytes = 8ULL * 1024 * 1024 * 1024;
        std::optional<std::uint64_t> expected;
        int index = 2;
        if (!shell) { if (index >= argc) return 2; url = argv[index++]; }
        while (index < argc) {
            const std::string option = argv[index++];
            if (option == "--allow-http") { config.allow_http = 1; continue; }
            if (index >= argc) return 2;
            const std::string value = argv[index++];
            if (option == "--root") root = value;
            else if (option == "--key") key = value;
            else if (option == "--ca-bundle") ca_bundle = value;
            else if (option == "--max-bytes") config.max_download_bytes = number(value);
            else if (option == "--expected-length") expected = number(value);
            else if (option == "--pause-after") pause_after = number(value);
            else return 2;
        }
        if (root.empty()) return 2;
        config.storage_root = fetchkit::stringView(root);
        config.ca_bundle = fetchkit::stringView(ca_bundle);
        fetchkit::Engine engine(config);
        if (shell) {
            std::unordered_map<std::uint64_t, fetchkit::Task> tasks;
            std::string line;
            while (std::getline(std::cin, line)) {
                std::istringstream input(line);
                std::string command; input >> command;
                if (command.empty()) continue;
                if (command == "quit") break;
                try {
                    if (command == "enqueue") {
                        std::string task_key, address;
                        if (!(input >> std::quoted(task_key) >> std::quoted(address))) throw std::runtime_error("InvalidArgument");
                        auto task = engine.enqueue(requestFor(address, task_key));
                        const auto id = task.query().task_id;
                        tasks.emplace(id, std::move(task));
                        std::cout << "{\"accepted\":" << id << "}\n";
                    } else {
                        std::uint64_t id = 0; input >> id;
                        auto it = tasks.find(id);
                        if (it == tasks.end()) throw std::runtime_error("Closed");
                        if (command == "query") { printSnapshot(std::cout, it->second.query()); std::cout << '\n'; }
                        else if (command == "pause" || command == "resume" || command == "cancel") {
                            const auto status = command == "pause" ? it->second.pause() : command == "resume" ? it->second.resume() : it->second.cancel();
                            std::cout << "{\"status\":" << jsonString(fetchkit_status_name(status)) << "}\n";
                        } else if (command == "release") { tasks.erase(it); std::cout << "{\"status\":\"Ok\"}\n"; }
                        else throw std::runtime_error("InvalidArgument");
                    }
                } catch (const std::exception& error) { std::cout << "{\"error\":" << jsonString(error.what()) << "}\n"; }
                std::cout.flush();
            }
            return 0;
        }
        auto request = requestFor(url, key);
        if (expected) { request.expected_length_known = 1; request.expected_length = *expected; }
        auto task = engine.enqueue(request);
        std::signal(SIGINT, interrupt);
        bool paused = false;
        auto next_progress = std::chrono::steady_clock::now();
        while (true) {
            const auto snapshot = task.query();
            if (interrupted) { task.cancel(); interrupted = 0; }
            if (pause_after && !paused && snapshot.received_bytes >= pause_after && !stopped(snapshot)) {
                if (task.pause() == FETCHKIT_OK) {
                    const auto result = task.wait(60000);
                    if (result.state == FETCHKIT_PAUSED) fetchkit::checkStatus(task.resume());
                    paused = true;
                    continue;
                }
            }
            if (stopped(snapshot)) { printSnapshot(std::cout, snapshot); std::cout << '\n'; return exitCode(snapshot); }
            if (std::chrono::steady_clock::now() >= next_progress) {
                std::cerr << "Staged " << snapshot.received_bytes;
                if (snapshot.total_known) std::cerr << '/' << snapshot.total_bytes;
                std::cerr << " bytes\n";
                next_progress = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    } catch (const std::exception& error) {
        std::cerr << "FetchKit: " << error.what() << '\n';
        return std::string(error.what()) == "InvalidArgument" ? 2 : 8;
    }
}
