#pragma once

#include "core/ports/random.hpp"
#include "core/ports/storage.hpp"
#include "core/util/time.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace infra::s3util {

[[nodiscard]] bool is_retryable(core::ports::StorageError error) noexcept;

// Delta-seconds only. S3-compatible services do not send the HTTP-date form, and a value
// that cannot be read is ignored rather than trusted.
[[nodiscard]] std::optional<core::Seconds> parse_retry_after(std::string_view value) noexcept;

// Capped exponential backoff with full jitter: each delay is uniform in
// [0, min(cap, base * 2^retries_done)], so clients that failed together do not retry together.
class RetryPolicy {
public:
    struct Config {
        core::Millis base;
        core::Millis cap;
        std::uint32_t max_retries = 0;
    };

    // A non-positive base or a cap below it is a configuration bug and throws
    // std::invalid_argument.
    explicit RetryPolicy(const Config& config);

    // The pause before the next attempt, or nullopt to give up. A Retry-After is a floor on
    // the pause; one longer than the cap gives up instead, because a server that wants us
    // away for longer than we would ever wait is as good as down.
    [[nodiscard]] std::optional<core::Millis> next_delay(std::uint32_t retries_done,
                                                         core::ports::StorageError error,
                                                         std::optional<core::Seconds> retry_after,
                                                         core::ports::IRandom& random) const;

private:
    Config config_;
};

} // namespace infra::s3util
