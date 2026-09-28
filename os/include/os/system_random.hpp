#pragma once

#include "core/ports/random.hpp"

#include <cstddef>
#include <span>

namespace os {

// getrandom(2) from the kernel CSPRNG. Blocks only before the pool is first seeded, which
// happens during early boot, never while serving.
class SystemRandom final : public core::ports::IRandom {
public:
    void fill(std::span<std::byte> out) noexcept override;
};

} // namespace os
