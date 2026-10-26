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

The Linux CI entry is `bash tools/ci/linux_m1.sh`; set `FETCHKIT_RUN_LARGE_FILE=1` to include the actual
>4 GiB transfer. A missing Linux execution record remains a missing acceptance gate, even when the
same script/protocol code passes on Windows.
