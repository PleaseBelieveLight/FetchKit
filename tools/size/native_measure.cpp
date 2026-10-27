#include "fetchkit/version.h"
#ifdef FETCHKIT_MEASURE_DIGEST
#include "storage/resource_store.h"
#include <cstring>
#endif

#if defined(_WIN32)
#define FETCHKIT_MEASURE_EXPORT __declspec(dllexport)
#else
#define FETCHKIT_MEASURE_EXPORT __attribute__((visibility("default")))
#endif

// Measurement-only entry: retain the actual file verifier in a stripped library.
extern "C" FETCHKIT_MEASURE_EXPORT int fetchkit_measure(const char* path,char* digest) noexcept {
#ifdef FETCHKIT_MEASURE_DIGEST
    try {
        if(!path || !digest) return 1;
        fetchkit::storage::StagingFile file(std::filesystem::u8path(path),false,true);
        const auto hash=fetchkit::storage::sha256File(file);
        std::memcpy(digest,hash.c_str(),65);return 0;
    }catch(...){return 1;}
#else
    (void)path;(void)digest;return fetchkit::version()[0] ? 0 : 1;
#endif
}
