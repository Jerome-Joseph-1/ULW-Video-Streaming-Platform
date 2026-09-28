#pragma once

#include "core/ports/clock.hpp"

namespace os {

class SystemClock final : public core::ports::IClock {
public:
    [[nodiscard]] core::MonoTime now() const noexcept override;
    [[nodiscard]] core::WallTime wall_now() const noexcept override;
};

} // namespace os
