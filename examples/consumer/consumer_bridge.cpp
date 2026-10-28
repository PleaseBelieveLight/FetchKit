#include <fetchkit/fetchkit.h>

// Also verifies that the installed static dependency closure can link into a shared library.
extern "C" fetchkit_status consumer_open_close(const fetchkit_engine_config* config) noexcept {
    fetchkit_engine engine = 0;
    const auto status = fetchkit_engine_create(config, &engine);
    return status == FETCHKIT_OK ? fetchkit_engine_close(engine) : status;
}
