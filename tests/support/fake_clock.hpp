#pragma once

#include "core/ports/clock.hpp"
#include "core/util/time.hpp"

#include <chrono>

namespace ulw::test {

// Starts at a fixed instant so anything derived from time is reproducible.
class FakeClock final : public core::ports::IClock {
public:
    [[nodiscard]] core::MonoTime now() const noexcept override { return mono_; }
    [[nodiscard]] core::WallTime wall_now() const noexcept override { return wall_; }

    void advance(core::Millis d) noexcept {
        mono_ += d;
        wall_ += d;
    }

private:
    core::MonoTime mono_{std::chrono::hours(1000)};
    // 2026-01-01T00:00:00Z
    core::WallTime wall_{std::chrono::seconds(1767225600)};
};

} // namespace ulw::test
