# Native reference dependencies

`curl.lock.json` pins the reference curl source archive and SHA-256. The archive was obtained from
curl's official HTTPS download endpoint. `FETCHKIT_USE_PINNED_CURL=ON` verifies the hash before
CMake extracts/builds it; `FETCHKIT_CURL_ARCHIVE` accepts an offline copy with the same checksum.
No archive or prebuilt binary is committed. The upstream license is in `licenses/curl.txt`.

The acceptance profile enables HTTP/HTTPS, OpenSSL and threaded DNS, and disables optional protocol,
compression, IDN, PSL, SSH, HTTP/2 and tool/test/doc dependencies. This profile still depends on the
host's compiler, OpenSSL, libc and CA store. Their versions must be recorded with acceptance results;
pinning curl alone does not promise bit-for-bit binaries or a complete dependency lock.

`FETCHKIT_USE_PINNED_CURL=OFF` permits a system libcurl >=7.85 for development. Record its actual
version/features and do not describe that mode as the source-pinned reference profile. Android's
ordinary platform build never fetches or links curl.

M2 vendors the unmodified MIT-licensed PicoSHA2 header at the commit and file hashes in
`picosha2.lock.json`. Upstream: https://github.com/okdshin/PicoSHA2 ; license: `licenses/picosha2.txt`.
The verifier rereads bounded blocks and does not serialize internal hash state. No TLS library is
introduced solely for SHA-256. `record.cpp` also uses SHA-256 to derive stable storage identities.
Record CRC32 uses the platform zlib (`find_package(ZLIB)` on hosts; Android's system `libz`).
Record the installed zlib version/runtime dependency with measurements; it is not source-pinned.
The header hash is checked during CMake configuration to catch accidental vendor drift.

The Linux CI entry is `bash tools/ci/linux_m1.sh`; set `FETCHKIT_RUN_LARGE_FILE=1` to include the actual
>4 GiB transfer. A missing Linux execution record remains a missing acceptance gate, even when the
same script/protocol code passes on Windows.

M2 acceptance: `bash tools/ci/linux_m2.sh`. Run `FETCHKIT_SANITIZERS=address,undefined` and
`FETCHKIT_SANITIZERS=thread` separately for memory/undefined-behavior and concurrency diagnostics.
`FETCHKIT_RUN_LARGE_FILE=1` also enables real >4 GiB recovery and SDK digest verification; optimized
builds are recommended for this CPU-intensive test. No script installs software or changes the OS.

M3 uses the same external dependencies and adds no SDK runtime library. Run `bash tools/ci/linux_m3.sh`
for scheduling/cache acceptance. Retry-After parsing uses bounded ASCII fields and UTC arithmetic;
it does not change global locale/timezone or add a date/serialization dependency.

M4 makes the previously evaluated OkHttp 5.5.1 client a production Android runtime dependency.
OkHttp/Okio/Kotlin/AndroidX retain their upstream Apache-2.0 licensing; Maven artifacts supply their
metadata and consumer rules. No networking JAR is embedded in the AAR. A direct file-AAR consumer
must also declare the documented OkHttp dependency graph. The SDK does not add WorkManager or Compose.
`tools/size/measure_m4.py` records the resolved dependencies, complete JNI/AAR/APK cost and R8 comparison.
Test TLS keys/certificates are synthetic fixtures; only the sample Debug variant trusts the local certificate.

M5 archives resolved dependency metadata, embedded license/notice files and the NDK NOTICE alongside
the local preview. Missing cached license texts require completion and review before distribution.
The owner selected Apache-2.0 for FetchKit on 2026-10-28; the canonical project text is in the root `LICENSE`.
Third-party licenses remain separate. Native installs, AAR Java resources, sources JARs and the preview
retain the project license; the Maven POM declares it. Complete third-party license review remains a release gate.
`VERSION` controls native and Maven artifact versions.
See `docs/m5-release.md` for the complete dependency and symbol archive procedure.
