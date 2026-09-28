#include "os/system_clock.hpp"

#include "core/util/time.hpp"

#include <chrono>

namespace os {

core::MonoTime SystemClock::now() const noexcept {
    return std::chrono::steady_clock::now();
}

core::WallTime SystemClock::wall_now() const noexcept {
    return std::chrono::system_clock::now();
}

} // namespace os
