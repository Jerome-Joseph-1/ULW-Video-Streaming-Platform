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

} // namespace chat
