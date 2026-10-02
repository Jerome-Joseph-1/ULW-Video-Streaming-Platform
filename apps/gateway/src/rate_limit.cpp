#include "rate_limit.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace gateway {

std::expected<void, core::Millis> TokenBucket::take(const BucketRule& rule, core::MonoTime now,
                                                    double n) noexcept {
    if (now > last_refill_) {
        const double elapsed = std::chrono::duration<double>(now - last_refill_).count();
        tokens_ = std::min(rule.burst, tokens_ + (elapsed * rule.per_second));
        last_refill_ = now;
    }
    if (tokens_ >= n) {
        tokens_ -= n;
        return {};
    }
    const double seconds = (n - tokens_) / rule.per_second;
    return std::unexpected(core::Millis{static_cast<core::Millis::rep>(std::ceil(seconds * 1000))});
}

void TokenBucket::refund(const BucketRule& rule, double n) noexcept {
    tokens_ = std::min(rule.burst, tokens_ + n);
}

std::chrono::seconds retry_after(core::Millis wait) noexcept {
    const auto whole = std::chrono::ceil<std::chrono::seconds>(wait);
    return std::max(whole, std::chrono::seconds{1});
}

} // namespace gateway
