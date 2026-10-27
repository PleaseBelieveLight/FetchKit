# Repository Guidelines

## Documentation Synchronization

Chinese version: [AGENTS.zh-CN.md](AGENTS.zh-CN.md). Update both files together whenever either
changes; keep requirements, examples, and commands equivalent.

## Project Structure & Module Organization

FetchKit has an M3 host downloader with bounded scheduling, independent subscriptions, persisted
retries, quota/LRU/pin/expiry management, verified publication and read leases. Android remains a scaffold.
Read `README.md`, `docs/v1-design.md`, `docs/m3-host-api.md` and `docs/storage-record-v2.md`
before changing scope or architecture.

The root CMake project builds `include/fetchkit/`, `src/core/`, and `examples/cli/`.
Root Gradle settings map `:sdk` to `platform/android/sdk/` and `:sample` to
`platform/android/sample/`. Keep JNI and Kotlin in the SDK; Compose UI belongs in the sample.
Create storage, cache, transport, and protocol-test directories when implementing those capabilities.

## Build, Test, and Development Commands

From the repository root on Windows:

```powershell
.\gradlew.bat :sdk:assembleDebug :sample:assembleDebug :sample:testDebugUnitTest
.\gradlew.bat :sample:connectedDebugAndroidTest
cmake -S . -B build/native-debug -G Ninja -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Debug
cmake --build build/native-debug --parallel
cmake -S . -B build/m0-host -G Ninja -DCMAKE_CXX_COMPILER=g++ -DFETCHKIT_BUILD_TESTS=ON -DFETCHKIT_BUILD_PROBES=ON -DFETCHKIT_WITH_CURL=ON
cmake --build build/m0-host --parallel
ctest --test-dir build/m0-host --output-on-failure -LE large_file
```

These build AAR/APK artifacts, run unit/device tests, and compile the native CLI. Device tests
require a connected device. Run `build/native-debug/fetchkit_cli.exe --version` for native verification.
M0 probes require Python 3.10+ and libcurl 7.85+ development files; set `CMAKE_PREFIX_PATH` if needed.
See `tests/README.md` for opt-in Android probes and `tools/size/README.md` for size measurements.
M1 reference builds can enable `FETCHKIT_USE_PINNED_CURL`; `tools/ci/linux_m1.sh` runs the Linux
acceptance entry. Actual >4 GiB tests require `FETCHKIT_BUILD_LARGE_FILE_TESTS=ON` and the
`large_file` label. Never substitute Windows evidence for Linux acceptance.
M2 recovery tests also require `FETCHKIT_ENABLE_FAULT_INJECTION=ON` in a host test build;
ordinary SDK builds must keep it off. `tools/ci/linux_m2.sh` includes recovery; use separate
`FETCHKIT_SANITIZERS=address,undefined` and `FETCHKIT_SANITIZERS=thread` runs on Linux.
Host builds require zlib development files; Android uses the system libz. Vendored PicoSHA2
is hash-pinned with its MIT license in `third_party/`.
M3 Linux acceptance uses `tools/ci/linux_m3.sh`. Host protocol tests inspect only their own child
processes for memory/thread metrics. The event executor is host-pumped through `fetchkit_task_dispatch`;
do not introduce unbounded queues or call host code under SDK locks.

## Coding Style & Naming Conventions

Use `fetchkit` for the root directory, repository, CMake project, and C++ namespace; name the CLI
`fetchkit_cli` and prefix project build options with `FETCHKIT_`.

The SDK package is fixed at `com.luofelix.fetchkit`. The sample namespace is
`com.luofelix.fetchkit.sample`; its applicationId remains `com.luofelix.fetchkit`.

For new code, use C++17, four-space indentation, `PascalCase` types, `lowerCamelCase` functions, and
`snake_case` filenames and fields. Use RAII for ownership and 64-bit file offsets. Prevent
exceptions from crossing C ABI or JNI boundaries.

No formatter/linter is configured; include configuration when introducing one. Update design
documents when architecture changes.

## Testing Guidelines

Android uses JUnit4. Sample instrumentation verifies application identity and SDK JNI loading.
CTest registers the fixture-server, pure C ABI, fake-transport session, version, real protocol and
storage golden/SHA vectors, deterministic subprocess recovery, and optional curl contract/large-file tests.
No third-party C++ test framework or coverage threshold is configured. Protocol tests should
use filenames such as `resume_test.cpp` and the controllable local HTTP server.

Keep M0 experiment sources and dependencies out of ordinary SDK builds. Android probes are enabled
only with `-Pfetchkit.probes=true`; an experimental backend is selected with `fetchkit.probeBackend`.
Report failed contract gates and missing environments explicitly instead of weakening assertions.

Cover ignored Range requests, invalid
206/416 responses, resource changes, interrupted checkpoints, checksum failures, disk exhaustion,
and cancellation races. Follow the acceptance checklist in `docs/v1-design.md`; distinguish process
termination tests from reboot tests.
Only Completed objects may be acquired. Do not restore callbacks/credentials from records or
advance committed offsets before data and record synchronization. Retain unknown-version data.
Use record v2 for retry/pin/expiry semantics; migrate v1 and never silently discard critical fields.
Reserve quota before writes, count metadata and unknown files, and protect leases/active tasks from removal.
Cancellation revokes one subscription; only the last explicit cancellation cleans the shared unfinished task.

## Commit & Pull Request Guidelines

Use `type: description`, e.g. `refactor: separate SDK and sample`. Keep subjects focused on the resulting change.

PRs explain behavior, scope, validation, and linked issues. Bug fixes summarize cause and repair.
Report dependency/size changes with measurements. After code changes, suggest a commit message.

## Architecture & Security

Keep download policy and storage in C++; isolate platform networking and scheduling. Publish files
only after required validation. Redact credentials and signed URLs. Measure package-size claims with
all newly required dependencies included.
