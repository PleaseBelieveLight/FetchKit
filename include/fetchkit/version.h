#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Returns a library-owned string valid for the lifetime of the process. */
const char* fetchkit_version(void);

#ifdef __cplusplus
}

namespace fetchkit {

inline const char* version() noexcept {
    return fetchkit_version();
}

}
#endif
