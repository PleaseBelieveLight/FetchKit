#include "storage/resource_store.h"
#include "storage/sha256.h"
#include <array>

namespace fetchkit::storage {
std::string sha256File(StagingFile& file, transport::Cancellation* cancellation) {
    picosha2::hash256_one_by_one hash;
    std::array<std::uint8_t,65536> buffer{};
    file.seek(0);
    for (;;) {
        if (cancellation && cancellation->isCancelled()) throw core::Failure(FETCHKIT_CANCELLED);
        const auto count=file.read(buffer.data(),buffer.size());
        if (!count) break;
        hash.process(buffer.begin(),buffer.begin()+count);
    }
    hash.finish();
    return hashHex(hash);
}

std::uint32_t verifyFile(const std::filesystem::path& path, const Record& r, transport::Cancellation* cancellation) {
    if (!regularFile(path)) throw core::Failure(FETCHKIT_INTEGRITY_MISMATCH);
    StagingFile file(path,false,true);
    if (file.size()!=r.committed || (r.total && *r.total!=r.committed) ||
        (r.request.expected_length && *r.request.expected_length!=r.committed)) throw core::Failure(FETCHKIT_INTEGRITY_MISMATCH);
    if (!r.request.expected_sha256.empty()) {
        if (sha256File(file,cancellation)!=r.request.expected_sha256) throw core::Failure(FETCHKIT_INTEGRITY_MISMATCH);
        return FETCHKIT_DIGEST_VERIFIED;
    }
    if (r.request.require_digest) throw core::Failure(FETCHKIT_INTEGRITY_MISMATCH);
    if (cancellation && cancellation->isCancelled()) throw core::Failure(FETCHKIT_CANCELLED);
    return FETCHKIT_TRANSPORT_COMPLETE;
}
}
