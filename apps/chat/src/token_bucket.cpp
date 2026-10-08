#include "token_bucket.hpp"

#include <algorithm>
#include <chrono>

namespace chat {

namespace {

constexpr std::uint64_t kScale = 1000;

} // namespace

TokenBucket::TokenBucket(std::uint32_t burst, std::uint32_t per_second, core::MonoTime now) noexcept
    : capacity_(std::uint64_t{burst} * kScale), per_second_(per_second), level_(capacity_),
      refilled_(now) {}

// Thousandths of a token per millisecond is tokens per second: whole milliseconds earn an
// exact amount, and the fraction of one not yet counted stays on the clock for next time.
std::uint64_t TokenBucket::level(core::MonoTime now) const noexcept {
    const auto elapsed = std::chrono::duration_cast<core::Millis>(now - refilled_).count();
    if (elapsed <= 0) {
        return level_;
    }
    return std::min(capacity_, level_ + (static_cast<std::uint64_t>(elapsed) * per_second_));
}

void TokenBucket::refill(core::MonoTime now) noexcept {
    const auto elapsed = std::chrono::duration_cast<core::Millis>(now - refilled_);
    if (elapsed.count() <= 0) {
        return;
    }
    level_ = level(now);
    refilled_ += elapsed;
}

std::expected<void, core::Millis> TokenBucket::take(core::MonoTime now) noexcept {
    refill(now);
    if (level_ >= kScale) {
        level_ -= kScale;
        return {};
    }
    if (per_second_ == 0) {
        return std::unexpected(core::Millis::max());
    }
    // Rounded up: a client that waits exactly this long finds a whole token.
    const std::uint64_t missing = kScale - level_;
    return std::unexpected(
        core::Millis{static_cast<core::Millis::rep>((missing + per_second_ - 1) / per_second_)});
}

void TokenBucket::give_back() noexcept {
    level_ = std::min(capacity_, level_ + kScale);
}

bool TokenBucket::full(core::MonoTime now) const noexcept {
    return level(now) == capacity_;
}

bool PacedBucket::available(core::MonoTime now) const noexcept {
    // Full at `now` holds burst; each whole interval still to go before full is one fewer, and
    // the last is taken when burst - 1 are still owed.
    return burst_ > 0 && due_ <= now + (interval_ * (burst_ - 1));
}

core::Millis PacedBucket::wait(core::MonoTime now) const noexcept {
    if (burst_ == 0) {
        return core::Millis::max();
    }
    const core::MonoTime at = due_ - (interval_ * (burst_ - 1));
    if (at <= now) {
        return core::Millis{0};
    }
    // Rounded up: a client that waits exactly this long finds the allowance there.
    return std::chrono::ceil<core::Millis>(at - now);
}

void PacedBucket::take(core::MonoTime now) noexcept {
    due_ = std::max(due_, now) + interval_;
}

} // namespace chat
