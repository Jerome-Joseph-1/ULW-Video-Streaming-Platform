#include "infra/s3util/retry.hpp"

#include "core/ports/random.hpp"
#include "core/ports/storage.hpp"
#include "core/util/time.hpp"

#include "decimal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace infra::s3util {

bool is_retryable(core::ports::StorageError error) noexcept {
    using core::ports::StorageError;
    switch (error) {
    case StorageError::Transient:
    case StorageError::Throttled:
        return true;
    case StorageError::NotFound:
    case StorageError::AlreadyExists:
    case StorageError::PreconditionFailed:
    case StorageError::Unauthorized:
    case StorageError::Permanent:
    case StorageError::Corrupt:
        return false;
    }
    return false;
}

std::optional<core::Seconds> parse_retry_after(std::string_view value) noexcept {
    return detail::parse_decimal<std::uint32_t>(value).transform(
        [](std::uint32_t s) { return core::Seconds{s}; });
}

RetryPolicy::RetryPolicy(const Config& config) : config_(config) {
    if (config.base <= core::Millis::zero() || config.cap < config.base) {
        throw std::invalid_argument("retry policy needs 0 < base <= cap");
    }
}

std::optional<core::Millis> RetryPolicy::next_delay(std::uint32_t retries_done,
                                                    core::ports::StorageError error,
                                                    std::optional<core::Seconds> retry_after,
                                                    core::ports::IRandom& random) const {
    if (!is_retryable(error) || retries_done >= config_.max_retries ||
        (retry_after && *retry_after > config_.cap)) {
        return std::nullopt;
    }
    core::Millis ceiling = config_.base;
    // Stops doubling at the cap, so a large retry count can never overflow.
    for (std::uint32_t i = 0; i < retries_done && ceiling < config_.cap; ++i) {
        ceiling = ceiling > config_.cap / 2 ? config_.cap : ceiling * 2;
    }
    std::array<std::byte, sizeof(std::uint64_t)> bytes{};
    random.fill(bytes);
    // Modulo bias is below (ceiling + 1) / 2^64, far too small to show in a delay.
    const std::uint64_t draw =
        std::bit_cast<std::uint64_t>(bytes) % (static_cast<std::uint64_t>(ceiling.count()) + 1);
    const core::Millis delay{static_cast<core::Millis::rep>(draw)};
    if (retry_after) {
        return std::max(delay, core::Millis{*retry_after});
    }
    return delay;
}

} // namespace infra::s3util
