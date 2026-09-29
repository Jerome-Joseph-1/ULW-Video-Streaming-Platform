#pragma once

#include "core/models/ids.hpp"

#include <cstddef>
#include <cstdint>

namespace infra::postgres {

// The advisory lock key of an upload's claim. A UUIDv7 opens with 48 bits of millisecond
// timestamp, so its first 63 bits would tell uploads created in the same millisecond apart by
// barely a dozen random bits. The key comes from the random tail instead: bytes 8-15 hold
// rand_b's 62 bits under the 2 variant bits, and the low bit of byte 7 (rand_a) makes 63, a
// non-negative bigint. Two live uploads share a key with probability 2^-63, and sharing one
// only refuses claims on one while the other is held.
[[nodiscard]] inline std::int64_t lock_key(const core::UploadId& id) noexcept {
    const auto bytes = id.uuid().bytes();
    std::uint64_t tail = 0;
    for (std::size_t i = 8; i < bytes.size(); ++i) {
        tail = (tail << 8U) | std::to_integer<std::uint64_t>(bytes[i]);
    }
    constexpr std::uint64_t kRandB = (std::uint64_t{1} << 62U) - 1;
    const std::uint64_t rand_a_bit = std::to_integer<std::uint64_t>(bytes[7]) & 1U;
    return static_cast<std::int64_t>((tail & kRandB) | (rand_a_bit << 62U));
}

} // namespace infra::postgres
