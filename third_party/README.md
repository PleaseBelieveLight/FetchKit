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
