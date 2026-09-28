#pragma once

#include <cstddef>
#include <expected>

namespace os {

struct NofileLimits {
    std::size_t soft;
    std::size_t hard;
};

// Raises the soft RLIMIT_NOFILE to min(hard, cap). The cap exists because containers often
// allow a hard limit of 2^20, and every descriptor number costs a slot in the reactor's
// table; the kernel never hands out a descriptor at or above the soft limit.
[[nodiscard]] std::expected<NofileLimits, int> raise_nofile_limit(std::size_t cap) noexcept;

} // namespace os
