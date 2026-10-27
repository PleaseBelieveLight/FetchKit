#pragma once
#include <chrono>
#include <cstdint>
#include <memory>

namespace fetchkit::core {
class Clock {
public:
    virtual ~Clock() = default;
    virtual std::uint64_t nowMs() const = 0;
    virtual std::uint64_t steadyMs() const {return nowMs();}
};

class SystemClock final : public Clock {
public:
    std::uint64_t nowMs() const override {
        const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return value < 0 ? 0 : static_cast<std::uint64_t>(value);
    }
    std::uint64_t steadyMs() const override {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
};
}
