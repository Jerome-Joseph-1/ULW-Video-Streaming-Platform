#pragma once

#include "core/models/ids.hpp"
#include "core/util/time.hpp"
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

// A keyed hash for the rate-limit tables, whose keys (client addresses, user ids) the clients
// choose. A fixed hash would let them pick keys that all land in one run of the table and turn
// every lookup into a scan of it; a secret seed per process takes that choice away.
class SeededHash {
public:
    explicit SeededHash(std::uint64_t seed) noexcept : seed_(seed) {}
    [[nodiscard]] std::uint64_t operator()(std::span<const std::byte> bytes) const noexcept;

private:
    std::uint64_t seed_;
};

struct AddressHash {
    SeededHash hash;
    [[nodiscard]] std::uint64_t operator()(const net::IpAddress& a) const noexcept {
        return hash(std::as_bytes(std::span(a.bytes())));
    }
};

struct UserHash {
    SeededHash hash;
    [[nodiscard]] std::uint64_t operator()(const core::UserId& u) const noexcept {
        return hash(std::as_bytes(std::span(u.view())));
    }
};

// What the per-address limits count against. An IPv4 address is one client, or one NAT; an
// IPv6 client is handed a /64 at the least (RFC 6177's end-site advice) and can pick any
// address in it, so per-address limits on the full 128 bits would give it 2^64 fresh buckets.
[[nodiscard]] inline net::IpAddress client_key(const net::IpAddress& address) noexcept {
    constexpr unsigned kIpv6Site = 64;
    return address.is_v4() ? address : address.prefix(kIpv6Site);
}

// The client behind a request a trusted proxy relayed: the `hops`-th X-Forwarded-For entry from
// the right, `hops` being the proxies in front of us, each of which appends the address it was
// reached from. Entries further left are whatever the client sent. Skipping every entry that
// looks like a proxy instead would trust addresses by where they fall: a proxy that sees its
// clients through a masquerade inside the trusted block would pass a client-written entry
// through. Too few entries, or a malformed one where the client should be, count the request
// against the peer itself, so a proxy set up otherwise than configured fails closed.
[[nodiscard]] net::IpAddress forwarded_client(const net::IpAddress& peer,
                                              std::span<const http::HeaderField> headers,
                                              std::size_t hops) noexcept;

} // namespace gateway
