# Repository Guidelines

## Documentation Synchronization

Chinese version: [AGENTS.zh-CN.md](AGENTS.zh-CN.md). Update both files together whenever either
changes; keep requirements, examples, and commands equivalent.

## Documentation Structure

- Keep the root `README.md` concise and limited to a project introduction, a minimal code example,
  an architecture overview and core SDK interfaces. Link to `docs/README.md` for further reading;
  put build commands, setup, sample operations, testing, release steps and milestone history in
  `docs/`.
- Use `docs/README.md` as the documentation entry point. Organize documents by topic:
  `guides/` for setup/development/sample usage, `reference/` for current APIs and protocol/storage
  contracts,
  `design/` for architecture/plans, `release/` for packaging/release gates, `reports/` for
  validation evidence, and `archive/` for superseded stage guides. Do not add loose topic pages
  directly under `docs/`.
- Use stable topic filenames for current guides and APIs (for example `reference/android-api.md`);
  keep milestone names in historical guides and reports. Mark historical material clearly and link
  to current APIs.
- Keep each document focused, with short explanations and the smallest useful examples. Maintain one
  authoritative location for each procedure or contract and link to it instead of copying it into
  multiple guides. Distinguish current behavior, design goals and historical evidence; preserve
  unverified limits and release gates.
- When adding, moving or renaming documents, update `docs/README.md`, relevant archive/report
  indexes, relative links, anchors and textual paths across the repository, including sample hints
  and both AGENTS files. Provide a route back to the documentation entry point from topic pages and
  check local links before finishing.
- Keep existing raw evidence paths and contents under `docs/reports/` unchanged during
  reorganization. Tool-local instructions may stay beside their tools (for example
  `tests/README.md`, `tools/size/README.md`
  and `third_party/README.md`); expose them through the documentation index instead of duplicating
  them.

## Project Structure & Module Organization

FetchKit has an M3 host downloader with bounded scheduling, independent subscriptions, persisted
retries, quota/LRU/pin/expiry management, verified publication and read leases. M4 adds asynchronous
Kotlin/JNI APIs, isolated OkHttp transport and Compose/JobScheduler examples. M5 adds opt-in CMake
install/export, local Maven publication, standalone consumers and release gates. Read `README.md`,
`docs/README.md`, `docs/design/v1-design.md`, `docs/reference/host-api.md`,
`docs/reference/android-api.md` and `docs/reference/storage-record-v2.md`
before changing scope or architecture. Read `docs/release/packaging.md` and
`docs/release/checklist.md` before changing packaging or release claims.

The root CMake project builds `include/fetchkit/`, `src/core/`, and `examples/cli/`. Root Gradle
settings map `:sdk` to `platform/android/sdk/` and `:sample` to
`platform/android/sample/`. Keep JNI and Kotlin in the SDK; Compose UI belongs in the sample. Create
storage, cache, transport, and protocol-test directories when implementing those capabilities.

## Build, Test, and Development Commands

From the repository root on Windows:

```powershell
.\gradlew.bat :sdk:assembleDebug :sdk:testDebugUnitTest :sample:assembleDebug :sample:testDebugUnitTest
.\gradlew.bat :sample:connectedDebugAndroidTest '-Pandroid.testInstrumentationRunnerArguments.class=com.luofelix.fetchkit.sample.SdkIntegrationTest'
cmake -S . -B build/native-debug -G Ninja -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Debug
cmake --build build/native-debug --parallel
cmake -S . -B build/m0-host -G Ninja -DCMAKE_CXX_COMPILER=g++ -DFETCHKIT_BUILD_TESTS=ON -DFETCHKIT_BUILD_PROBES=ON -DFETCHKIT_WITH_CURL=ON
cmake --build build/m0-host --parallel
ctest --test-dir build/m0-host --output-on-failure -LE large_file
```

These build AAR/APK artifacts, run unit/device tests, and compile the native CLI. Device tests
require a connected device. Run `build/native-debug/fetchkit_cli.exe --version` for native
verification. M0 probes require Python 3.10+ and libcurl 7.85+ development files; set
`CMAKE_PREFIX_PATH` if needed. See `tests/README.md` for opt-in Android probes and
`tools/size/README.md` for size measurements. M1 reference builds can enable
`FETCHKIT_USE_PINNED_CURL`; `tools/ci/linux_m1.sh` runs the Linux acceptance entry. Actual >4 GiB
tests require `FETCHKIT_BUILD_LARGE_FILE_TESTS=ON` and the
`large_file` label. Never substitute Windows evidence for Linux acceptance. M2 recovery tests also
require `FETCHKIT_ENABLE_FAULT_INJECTION=ON` in a host test build; ordinary SDK builds must keep it
off. `tools/ci/linux_m2.sh` includes recovery; use separate
`FETCHKIT_SANITIZERS=address,undefined` and `FETCHKIT_SANITIZERS=thread` runs on Linux. Host builds
require zlib development files; Android uses the system libz. Vendored PicoSHA2 is hash-pinned with
its MIT license in `third_party/`. M3 Linux acceptance uses `tools/ci/linux_m3.sh`. Host protocol
tests inspect only their own child processes for memory/thread metrics. The event executor is
host-pumped through `fetchkit_task_dispatch`; do not introduce unbounded queues or call host code
under SDK locks. M4 production device matrices use `tools/ci/run_m4_android.py` and
`tools/ci/run_m4_recovery.py`. Reuse an existing device; record API/ABI and distinguish SIGKILL,
force-stop, reboot and power loss. Use uniquely named `adb reverse --no-rebind` rules and clean up
only owned mappings. ADB 33.0.3 collides multiple `tcp:0` requests in its allow-list. Emulator
startup also inherits `http_proxy`; use `tools/ci/start_m4_emulator.ps1` for a controlled existing
AVD. OkHttp 5.5.1 is now a production runtime dependency. Test CA/network exceptions belong only in
sample Debug; SDK/Release artifacts must exclude probes, fault hooks and test trust anchors. Measure
the full file-AAR host.
`VERSION` is the shared native/Maven version. Use `FETCHKIT_INSTALL=ON` only in production builds;
fault injection and sanitizers must never be installed. Local AAR publication uses
`:sdk:publishReleasePublicationToPreviewRepository` and requires both ABIs with probes off. M5
acceptance uses `tools/ci/linux_m5.sh`, `verify_native_package.py`, `verify_android_package.py` and
`run_m5_stress.py`. `run_m5_enospc.py` requires Linux user/mount namespaces. Package previews with
`tools/release/package_preview.py`; match unstripped symbols by ELF Build ID and preserve SHA
manifests.
`run_m4_recovery.py --reboot` reboots only the explicitly selected emulator guest; physical reboot
and power loss require separate evidence. `tools/release/check_release.py` must block release when
required evidence is missing. FetchKit is licensed under Apache-2.0 in the root `LICENSE`; preserve
third-party licenses separately. Include the identical project license in native installs, AAR Java
resources and sources JARs, and declare it in the Maven POM. Do not upload artifacts without the
owner's release instructions.

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

## Comments & Logging

- Write code comments and human-readable log messages primarily in Chinese. Keep English for
  established technical terms or concepts that Chinese cannot express clearly; preserve API names,
  identifiers, protocol fields, error codes and machine-readable output formats.
- Document public C/C++/Kotlin API contracts at their declarations, using KDoc where appropriate:
  parameter bounds/units, result and error semantics, threading/callback constraints, ownership and
  release requirements. Explain shared-task effects of pause/resume, subscription cancellation
  versus release, provider cancellation and lease lifetime. Describe actual outcomes, including
  races.
- Add concise comments beside non-obvious invariants: lock order, generation fencing, cancellation
  barriers, data/record synchronization, publication/recovery ordering, quota protection and
  protocol workarounds. Explain the reason and failure being prevented. Update comments with
  behavior changes; remove obsolete milestone notes and commented-out code. Avoid narrating obvious
  statements or adding comments to every function.
- Add diagnostic logs at boundaries that help locate failures: engine/storage open or close
  failures, abnormal recovery/quarantine, exhausted retries or significant fallback decisions,
  validation or publication failures, and provider/listener/executor failures. Log once at the layer
  with enough context; retain the existing error/snapshot/event result. Do not silently discard
  unexpected exceptions without an observable error or safe diagnostic.
- Keep SDK logging quiet by default and allow the host to enable diagnostics and control levels. Use
  Error for failed operations, Warn for recoverable anomalies and Debug for diagnostic detail;
  expected pause/cancel actions are not errors. Avoid per-buffer, per-read/write, per-poll and
  per-progress logs. Sample or rate-limit repeated messages and bound any retained history.
- Include available, relevant context such as operation/stage, taskId/persistentId, generation,
  status, HTTP status, platform code and retry count. Do not log token, Cookie, authorization
  headers, full signed URLs or response bodies. Use explicitly selected safe fields; never dump
  request objects or raw exception messages/stacks without redaction, including through
  `toString()`.
- Keep log formatting and sinks lightweight. Do not invoke host logging callbacks under SDK locks or
  introduce blocking I/O/unbounded queues in hot paths. Keep CLI/test machine-readable output
  separate from diagnostic logs.

## Testing Guidelines

Android uses JUnit4. Sample instrumentation verifies application identity and SDK JNI loading. CTest
registers the fixture-server, pure C ABI, fake-transport session, version, real protocol and storage
golden/SHA vectors, deterministic subprocess recovery, and optional curl contract/large-file tests.
No third-party C++ test framework or coverage threshold is configured. Protocol tests should use
filenames such as `resume_test.cpp` and the controllable local HTTP server.

Keep M0 experiment sources and dependencies out of ordinary SDK builds. Android probes are enabled
only with `-Pfetchkit.probes=true`; an experimental backend is selected with
`fetchkit.probeBackend`. Report failed contract gates and missing environments explicitly instead of
weakening assertions.

Cover ignored Range requests, invalid 206/416 responses, resource changes, interrupted checkpoints,
checksum failures, disk exhaustion, and cancellation races. Follow the acceptance checklist in
`docs/design/v1-design.md`; distinguish process termination tests from reboot tests. Only Completed
objects may be acquired. Do not restore callbacks/credentials from records or advance committed
offsets before data and record synchronization. Retain unknown-version data. Use record v2 for
retry/pin/expiry semantics; migrate v1 and never silently discard critical fields. Reserve quota
before writes, count metadata and unknown files, and protect leases/active tasks from removal.
Cancellation revokes one subscription; only the last explicit cancellation cleans the shared
unfinished task.

## Commit & Pull Request Guidelines

Use `type: description`, e.g. `refactor: separate SDK and sample`. Keep subjects focused on the
resulting change.

PRs explain behavior, scope, validation, and linked issues. Bug fixes summarize cause and repair.
Report dependency/size changes with measurements. After code changes, suggest a commit message.

## Architecture & Security

Keep download policy and storage in C++; isolate platform networking and scheduling. Publish files
only after required validation. Redact credentials and signed URLs. Measure package-size claims with
all newly required dependencies included.
