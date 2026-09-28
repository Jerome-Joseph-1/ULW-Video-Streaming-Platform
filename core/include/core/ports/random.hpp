#pragma once

#include <cstddef>
#include <span>

namespace core::ports {

class IRandom {
public:
    virtual ~IRandom() = default;
    virtual void fill(std::span<std::byte> out) noexcept = 0;
};

} // namespace core::ports
