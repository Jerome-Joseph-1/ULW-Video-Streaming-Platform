#pragma once

#include "core/models/ids.hpp"
#include "core/util/time.hpp"
#include "http/client_limits.hpp"
#include "http/request.hpp"
#include "net/ip_address.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace gateway {

// How fast a bucket refills and how much it may save up.
struct BucketRule {
    double burst = 0;
    double per_second = 0;
};

// The classic {tokens, last_refill} pair: created full, refilled lazily on each use, so an idle
// bucket costs nothing and needs no timer.
class TokenBucket {
public:
    TokenBucket(const BucketRule& rule, core::MonoTime now) noexcept
        : tokens_(rule.burst), last_refill_(now) {}

    // Takes `n` tokens, or none, and then says how long until `n` would be there.
    [[nodiscard]] std::expected<void, core::Millis> take(const BucketRule& rule, core::MonoTime now,
                                                         double n = 1) noexcept;

    // Gives back what was taken for something that did not happen, never past the burst.
    void refund(const BucketRule& rule, double n) noexcept;

private:
    double tokens_;
    core::MonoTime last_refill_;
};

// Whole seconds for a Retry-After header, rounded up and never 0: "retry in 0 s" would invite
// the client straight back into the refusal.
[[nodiscard]] std::chrono::seconds retry_after(core::Millis wait) noexcept;

using http::AddressHash;
using http::client_key;
using http::forwarded_client;
using http::SeededHash;
using UserHash = http::ViewHash;

} // namespace gateway
