# Repository Guidelines

## Documentation Synchronization

Chinese version: [AGENTS.zh-CN.md](AGENTS.zh-CN.md). Update both files together whenever either
changes; keep requirements, examples and commands equivalent. Keep these files focused on durable
working rules. Put implementation history, environment workarounds and detailed procedures in
`docs/`
or the relevant tool README, and link to them here.

## Workflow & Required Reading

- Check the working tree before editing and preserve unrelated changes. Keep changes within the
  task; avoid incidental refactoring, mass formatting or renaming existing APIs to satisfy style
  rules.
- Start with [the documentation index](docs/README.md), then read the material relevant to the
  change:

| Change                                    | Required reading                                                                                                                                                                     |
|-------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Scope or architecture                     | [Design baseline](docs/design/核心设计与约束.md) and affected API contracts                                                                                                          |
| C/C++ API, scheduling or storage          | [Host API](docs/reference/C与C++接口与行为.md); [record v2](docs/reference/存储记录v2格式.md) for persistence changes, [record v1](docs/reference/存储记录v1与迁移.md) for migration |
| Kotlin/JNI, Android networking or sample  | [Android API](docs/reference/AndroidSDK接口与行为.md), [sample guide](docs/guides/Android示例操作指南.md) as applicable                                                              |
| Transport or HTTP behavior                | [Transport contract](docs/reference/传输后端契约.md) and the affected host/Android API                                                                                               |
| Packaging, dependencies or release claims | [Packaging](docs/release/SDK打包与发布.md), [release checklist](docs/release/发布验收清单.md), [dependency notes](third_party/README.md)                                             |

- Distinguish current contracts, design goals and historical evidence. If declarations,
  implementation, tests and documentation disagree, identify the mismatch and resolve it within the
  task; do not silently redefine behavior or weaken assertions to make a check pass.

## Project Structure & Ownership

- `include/fetchkit/` holds the public C ABI and C++ wrappers. The root CMake project builds
  `src/core/`, `src/storage/`, optional `src/transport/` backends and `examples/cli/`.
- Keep download policy, scheduling, retries, persistence and cache policy in C++. Isolate platform
  networking and system scheduling; do not duplicate core policy in Kotlin or the sample.
- Root Gradle settings map `:sdk` to `platform/android/sdk/` and `:sample` to
  `platform/android/sample/`. Kotlin/JNI and the isolated OkHttp adapter belong in the SDK; Compose
  UI and JobScheduler examples belong in the sample. The SDK must not require sample UI, WorkManager
  or notification components.
- Add modules only when needed by the implementation; do not create empty future directories.

## Documentation Structure

- Keep root `README.md` self-contained for basic SDK integration. Include the project overview,
  minimal setup and runnable examples, architecture, and core interfaces.
- Use `docs/README.md` as the index. Put usage and development guides in `guides/`, API and format
  contracts in `reference/`, architectural decisions and active plans in `design/`, packaging and
  release gates in `release/`, and measured results in `reports/`. Keep topic pages out of the
  `docs/` root. Keep tool-specific instructions beside their tools and link from the index.
- Use stable, descriptive Chinese filenames for new current topic pages, with established technical
  terms where appropriate. Keep conventional names such as `README.md` and `AGENTS.md`, and preserve
  historical report and evidence paths. Rename or move existing pages only as part of scoped
  documentation work.
- State the audience and purpose at the start. Use searchable headings and put the common workflow
  first. Give prerequisites and success criteria next to commands. Keep one authoritative section
  per procedure or contract. Other pages may include the minimum summary or example needed to
  complete their own task and should link directly to the authoritative section.
- Update the authoritative sections and affected examples when public behavior, user steps,
  architecture, or release requirements change. Internal changes with no effect on those areas
  need no docs update. Read historical reports only when the task needs their evidence.
- For additions, removals, or moves, update the index and affected links, anchors, and textual paths
  in both AGENTS files, scripts, workflows, tests, and sample hints as applicable. Give each topic
  page a route back to the index. Verify affected links, anchors, and examples. Before deleting an
  obsolete page, migrate any still-valid unique requirements and fix its references.
- Preserve raw evidence paths and contents. Use a distinct output path for each new evidence run.

## Coding Style & Naming

- Use `fetchkit` for the repository, CMake project and C++ namespace, `fetchkit_cli` for the CLI,
  and `FETCHKIT_` for project build options.
- The SDK package/namespace is `com.luofelix.fetchkit`. The sample namespace is
  `com.luofelix.fetchkit.sample`; its applicationId remains `com.luofelix.fetchkit`.
- Use C++17, four-space indentation, RAII and 64-bit file offsets. New C++ types use `PascalCase`,
  functions use `lowerCamelCase`, and filenames/fields use `snake_case`; preserve existing local
  conventions such as private member suffixes. Public C ABI names retain `fetchkit_` and
  `snake_case`.
- Kotlin types use `PascalCase`; functions and properties use `lowerCamelCase`. Match neighboring
  Kotlin filename conventions. Preserve existing public names and machine-readable formats.
- Prevent exceptions from crossing C ABI or JNI boundaries. No formatter/linter is configured;
  include its configuration if introducing one, and avoid unrelated reformatting.

## Core Correctness Invariants

- Keep workers, buffers, queues and retained events bounded. Host events are pumped through
  `fetchkit_task_dispatch`; never call host code under SDK locks or add an unbounded callback queue.
- Pause/resume affect a shared task. Cancellation revokes one subscription; only the last explicit
  cancellation cleans up the shared unfinished task. Last release stops I/O and attempts to preserve
  a checkpoint. Keep generation fencing and provider/callback cancellation barriers intact.
- Advance committed offsets only after data and record synchronization. Preserve publication and
  recovery ordering; only valid Completed objects may be acquired. Transport completion is distinct
  from trusted digest verification.
- Persist retry/pin/expiry semantics in record v2; migrate v1 without losing critical fields.
  Preserve unknown-version data. Never restore callbacks, providers or credentials from records.
- Reserve quota before writes, count metadata and unknown files, and protect active tasks, pins and
  read leases from removal/eviction. Engine close must preserve the documented lifetime of held
  leases.

## Comments & Logging

- Write code comments and human-readable logs primarily in Chinese. Keep established technical
  terms, API names, identifiers, protocol fields, error codes and machine-readable formats
  unchanged.
- Document public C/C++/Kotlin contracts at declarations, using KDoc where appropriate: parameter
  bounds/units, results/errors, threading/callback constraints, ownership and release requirements.
  Explain shared pause/resume, cancellation versus release, provider cooperation and lease lifetime,
  including races and actual possible outcomes.
- Comment non-obvious invariants: lock order, generation fencing, cancellation barriers, data/record
  synchronization, publication/recovery order, quota protection and protocol workarounds. Explain
  the failure prevented. Update comments with behavior; remove obsolete notes and commented-out
  code. Avoid narrating obvious statements or requiring comments on every function.
- Log useful failure boundaries: engine/storage open or close, abnormal recovery/quarantine,
  exhausted retries, significant fallbacks, validation/publication and provider/listener/executor
  failures. Log once where context is sufficient; preserve error/snapshot/event results. Unexpected
  exceptions must have an observable error or safe diagnostic rather than being silently discarded.
- SDK diagnostics are off by default and host-controlled. Use Error for failed operations, Warn for
  recoverable anomalies and Debug for detail. Expected pause/cancel actions are not errors. Avoid
  per-buffer, per-read/write, per-poll and per-progress logs; rate-limit repetition and bound
  history.
- Select safe context explicitly: operation/stage, generated taskId/persistentId, generation,
  status, HTTP/platform codes and retry count. Never log authentication tokens, Cookie,
  authorization headers, full signed URLs, response bodies, request dumps or unredacted exception
  messages/stacks, including indirectly through `toString()`.
- Keep formatting/sinks lightweight, outside SDK locks; do not add blocking I/O or unbounded queues
  to hot paths. Separate CLI/test machine-readable output from diagnostics.

## Validation

Use commands from [development](docs/guides/本地构建与测试.md) and [tests](tests/README.md),
executed
from the repository root. Select checks by affected behavior rather than running every historical
milestone entry for every edit:

| Change                          | Validation                                                                                                                                                                 |
|---------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Documentation only              | Local links/anchors, referenced paths, changed examples and `git diff --check`; no full native/Android build unless needed to validate an example                          |
| C++ core or C ABI               | Affected CTest unit/API tests; protocol/recovery tests for changed HTTP, storage or cancellation behavior                                                                  |
| Kotlin/JNI or Android transport | SDK build/unit tests and relevant device integration/protocol tests; host tests do not validate JNI or Android networking                                                  |
| Sample UI/lifecycle             | Sample build/unit tests; relevant instrumentation for interaction/lifecycle changes                                                                                        |
| Packaging or dependencies       | Native install/external C/C++ consumers and/or dual-ABI AAR/external Release R8 consumer, package-content/license checks and full dependency/size measurements as affected |

- Add or update regression coverage for logic changes and bug fixes; reuse the controllable local
  HTTP server and existing JUnit4/CTest infrastructure. Cover affected failure/race cases from the
  [design acceptance list](docs/design/核心设计与约束.md), not only the successful path. For
  low-impact
  wording/style changes, use relevant existing checks; avoid tests that merely repeat
  implementation.
- Check that expected tests actually ran; use `ctest --no-tests=error` for filtered runs and inspect
  Android test class/case counts. Report failed checks, missing environments and unexecuted tests
  explicitly; do not weaken gates or present compilation as runtime verification.
- Isolate host fault-injection/sanitizer builds from production. Recovery tests require
  `FETCHKIT_ENABLE_FAULT_INJECTION=ON`; run ASan/UBSan and TSan separately on Linux. Actual >4 GiB
  tests require `FETCHKIT_BUILD_LARGE_FILE_TESTS=ON` and the `large_file` label; simulated offsets
  are not actual large-file evidence. Linux acceptance uses `tools/ci/linux_m5.sh`.
- Record platform/API/ABI and distinguish Windows from Linux, emulators from physical devices,
  SIGKILL from force-stop, and process termination from reboot/power loss. Windows evidence cannot
  replace Linux acceptance, nor emulator results physical-device gates. Reuse existing devices; use
  the controlled AVD starter and Android drivers linked in the test guide. Clean up only owned
  processes and `adb reverse --no-rebind` mappings; measure only owned child processes. Automated
  reboot is limited to the explicitly selected emulator guest; physical reboot/power loss need
  separate evidence.

## Dependencies, Packaging & Release

- Read version values from `VERSION`, `gradle/libs.versions.toml` and dependency lock files instead
  of duplicating version snapshots here. Host builds require zlib; Android uses system libz.
  Preserve pinned third-party hashes and licenses, including PicoSHA2.
- Keep probe-only sources/dependencies out of ordinary SDK builds. Android probes require
  `-Pfetchkit.probes=true`, with `fetchkit.probeBackend` selecting the experiment. Production OkHttp
  is a separate runtime dependency. Test CA/network exceptions belong only in sample Debug;
  SDK/Release artifacts must exclude probes, fault hooks and test trust anchors.
- `FETCHKIT_INSTALL=ON` is opt-in for production-profile builds, including local previews; never
  install fault-injection or sanitizer builds. Local Maven publication uses
  `:sdk:publishReleasePublicationToPreviewRepository` with both supported ABIs and probes off.
- Follow [packaging](docs/release/SDK打包与发布.md) for external consumers, matched ELF Build ID
  symbols, dependency trees and SHA manifests. Measure the full host increment, including runtime
  and transitive dependencies; distinguish it from core-only size. Real ENOSPC evidence requires
  Linux user/mount namespaces and cannot be replaced by fault injection.
- FetchKit uses root `LICENSE` (Apache-2.0); preserve third-party licenses separately. Include the
  identical project license in native installs, AAR Java resources and sources JARs; declare it in
  the Maven POM. `tools/release/check_release.py` must block release when required evidence is
  missing; preview validation is not release acceptance. Do not upload artifacts without the owner's
  release instructions.

## Commit & Pull Request Guidelines

Use `type: description`, matching recent history; e.g. `docs: 规范文档维护与验证规则`. Keep subjects
focused on the resulting change. After project code changes, suggest a commit message.

PRs and task results explain changed behavior/scope, checks performed and remaining limitations. Bug
fixes summarize the cause and repair. Link related issues when available; dependency/size changes
include actual measurements. Do not claim unexecuted checks passed.
