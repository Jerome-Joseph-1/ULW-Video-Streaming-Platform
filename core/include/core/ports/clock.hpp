#pragma once

#include "core/util/time.hpp"

namespace core::ports {

// Deadlines and timeouts use now(); anything that leaves the process (UUIDv7 timestamps,
// JWT exp/nbf, presign dates) uses wall_now(). Tests advance both together.
class IClock {
public:
    virtual ~IClock() = default;
    [[nodiscard]] virtual MonoTime now() const noexcept = 0;
    [[nodiscard]] virtual WallTime wall_now() const noexcept = 0;
};

} // namespace core::ports
