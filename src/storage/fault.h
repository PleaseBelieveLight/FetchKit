#pragma once
#include <cstddef>

namespace fetchkit::storage {
// Compiled out of ordinary SDK builds. Deterministic subprocess test barriers.
void faultPoint(const char* name);
std::size_t writeLimit(std::size_t requested);
}
