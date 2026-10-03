#pragma once

#include "core/util/time.hpp"

#include <cstdint>
#include <expected>

namespace chat {

// `burst` tokens, refilled at `per_second`, on whatever clock the caller reads. Kept in
// thousandths of a token so that a refill of a few a second loses nothing to rounding between
// takes a few milliseconds apart.
class TokenBucket {
public:
    // Starts full: a user's first burst is theirs whenever it comes.
    TokenBucket(std::uint32_t burst, std::uint32_t per_second, core::MonoTime now) noexcept;

    // Takes a token, or says how long until there is one.
    [[nodiscard]] std::expected<void, core::Millis> take(core::MonoTime now) noexcept;
    // Returns a token taken for something that did not happen; never past the burst.
    void give_back() noexcept;
    // Full at `now`: forgetting the bucket would change nothing.
    [[nodiscard]] bool full(core::MonoTime now) const noexcept;

private:
    [[nodiscard]] std::uint64_t level(core::MonoTime now) const noexcept;
    void refill(core::MonoTime now) noexcept;

    std::uint64_t capacity_;
    std::uint32_t per_second_;
    std::uint64_t level_;
    core::MonoTime refilled_;
};

// `burst` at once, then one each `interval`: for allowances counted in minutes, which a
// per-second rate cannot express. Kept as the time the next one is due when the bucket is used
// up (a generic cell rate algorithm), so it is one time point and no arithmetic on fractions.
class PacedBucket {
public:
    PacedBucket(std::uint32_t burst, core::Millis interval, core::MonoTime now) noexcept
        : burst_(burst), interval_(interval), due_(now) {}

    // Whether take() would succeed at `now`, without taking.
    [[nodiscard]] bool available(core::MonoTime now) const noexcept;
    void take(core::MonoTime now) noexcept;
    // How long from `now` until take() would succeed: zero when it would now.
    [[nodiscard]] core::Millis wait(core::MonoTime now) const noexcept;
    [[nodiscard]] bool full(core::MonoTime now) const noexcept { return due_ <= now; }

private:
    std::uint32_t burst_;
    core::Millis interval_;
    // When the bucket is full again: every take moves it an interval on from `now` at the least.
    core::MonoTime due_;
};

} // namespace chat
