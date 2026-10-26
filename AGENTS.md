# Repository Guidelines

## Documentation Synchronization

Chinese version: [AGENTS.zh-CN.md](AGENTS.zh-CN.md). Update both files together whenever either
changes; keep requirements, examples, and commands equivalent.

## Project Structure & Module Organization

FetchKit has an M1 host staging downloader and an Android scaffold. Persistent recovery, publication,
scheduling and caching remain unimplemented. Read `README.md`, `docs/v1-design.md` and
`docs/m1-host-api.md` before changing scope or architecture.

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
optional curl contract/large-file tests. No third-party C++ test framework or coverage threshold is configured. Protocol tests should
use filenames such as `resume_test.cpp` and the controllable local HTTP server.

Keep M0 experiment sources and dependencies out of ordinary SDK builds. Android probes are enabled
only with `-Pfetchkit.probes=true`; an experimental backend is selected with `fetchkit.probeBackend`.
Report failed contract gates and missing environments explicitly instead of weakening assertions.

Cover ignored Range requests, invalid
206/416 responses, resource changes, interrupted checkpoints, checksum failures, disk exhaustion,
and cancellation races. Follow the acceptance checklist in `docs/v1-design.md`; distinguish process
termination tests from reboot tests.

## Commit & Pull Request Guidelines

Use `type: description`, e.g. `refactor: separate SDK and sample`. Keep subjects focused on the resulting change.

PRs explain behavior, scope, validation, and linked issues. Bug fixes summarize cause and repair.
Report dependency/size changes with measurements. After code changes, suggest a commit message.

## Architecture & Security

Keep download policy and storage in C++; isolate platform networking and scheduling. Publish files
only after required validation. Redact credentials and signed URLs. Measure package-size claims with
all newly required dependencies included.
