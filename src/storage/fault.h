#pragma once
#include <cstddef>
namespace fetchkit::transport {class Cancellation;}

namespace fetchkit::storage {
// Compiled out of ordinary SDK builds. Deterministic subprocess test barriers.
void faultPoint(const char* name, transport::Cancellation* cancellation = nullptr);
std::size_t writeLimit(std::size_t requested);
}
