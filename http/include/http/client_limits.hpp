#pragma once

#include "http/request.hpp"
#include "net/ip_address.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// What a server facing clients needs to hold each client to its own limits: who the client is
// (its address, or the one a trusted proxy names), and a bounded table to count it in. Shared by
// the gateway and chat_server (ADR-0052, ADR-0076).
namespace http {

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

// For keys that are text behind a view(), such as user ids.
struct ViewHash {
    SeededHash hash;
    template <class Key> [[nodiscard]] std::uint64_t operator()(const Key& key) const noexcept {
        return hash(std::as_bytes(std::span(key.view())));
    }
};

// What the per-address limits count against. An IPv4 address is one client, or one NAT; an
// IPv6 client is handed a /64 at the least (RFC 6177's end-site advice) and can pick any
// address in it, so per-address limits on the full 128 bits would give it 2^64 fresh buckets.
[[nodiscard]] inline net::IpAddress client_key(const net::IpAddress& address) noexcept {
    constexpr unsigned kIpv6Site = 64;
    return address.is_v4() ? address : address.prefix(kIpv6Site);
}

// The IPv6 block a client's /64 lies in, for a cap on all of one customer's /64s together: a
// provider may delegate a /56 or a /48 to one site (RFC 6177), and client_key counts each /64 in
// it afresh. nullopt for IPv4, whose one address is the client already.
[[nodiscard]] inline std::optional<net::IpAddress>
client_block(const net::IpAddress& address) noexcept {
    constexpr unsigned kIpv6Block = 48;
    if (address.is_v4()) {
        return std::nullopt;
    }
    return address.prefix(kIpv6Block);
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

// Every trusted block is tried against every accepted peer; a deployment names one or two.
// Trusted proxies as a list of CIDR blocks, comma separated, blanks around each allowed: at most
// 16, none of them /0 (every address on the internet could then name any client it liked). The
// reason says what is wrong, for a configuration error.
inline constexpr std::size_t kMaxTrustedProxies = 16;
[[nodiscard]] std::expected<std::vector<net::IpNetwork>, std::string>
parse_trusted_proxies(std::string_view text);

// Whether `peer` is one of `proxies`.
[[nodiscard]] bool is_trusted_proxy(std::span<const net::IpNetwork> proxies,
                                    const net::IpAddress& peer) noexcept;

// A map with a hard cap on its entries and no allocation once it has filled: the rate-limit
// state for clients and users, whose number an attacker chooses. Full, it makes room by
// dropping the least recently used entry that nothing has pinned; a pinned entry (one an open
// connection or a request in flight counts on) is never dropped, so its count can never be
// lost. Open addressing with linear probing over an index twice the capacity, and deletion by
// backward shift, which leaves no tombstones to slow later lookups.
template <class Key, class Value, class Hash> class BoundedTable {
public:
    using Slot = std::uint32_t;

    BoundedTable(std::size_t capacity, Hash hash)
        : hash_(std::move(hash)), capacity_(capacity), index_(std::bit_ceil(capacity * 2), kNone),
          mask_(index_.size() - 1) {
        nodes_.reserve(capacity);
    }

    // The entry for `key`, created from `make()` when missing, and now the most recently used.
    // nullopt when the table is full and every entry is pinned.
    template <class Make> [[nodiscard]] std::optional<Slot> acquire(const Key& key, Make&& make) {
        std::size_t at = home(key);
        for (; index_[at] != kNone; at = (at + 1) & mask_) {
            const Slot s = index_[at];
            if (nodes_[s].key == key) {
                if (nodes_[s].pins == 0) {
                    unlink(s);
                    link_front(s);
                }
                return s;
            }
        }
        Slot s = kNone;
        // Never past capacity_: the index, twice its size, then always has an empty place for
        // a probe to stop at.
        if (nodes_.size() < capacity_) {
            s = static_cast<Slot>(nodes_.size());
            nodes_.push_back(Node{.key = key, .value = std::forward<Make>(make)()});
        } else {
            if (idle_tail_ == kNone) {
                return std::nullopt;
            }
            s = idle_tail_;
            unlink(s);
            erase_index(s);
            ++evictions_;
            nodes_[s].key = key;
            nodes_[s].value = std::forward<Make>(make)();
            // Erasing may have shifted the run `key` probes, so its free place is found again.
            at = home(key);
            while (index_[at] != kNone) {
                at = (at + 1) & mask_;
            }
        }
        index_[at] = s;
        link_front(s);
        return s;
    }

    [[nodiscard]] Value& at(Slot s) noexcept { return nodes_[s].value; }
    [[nodiscard]] std::uint32_t pins(Slot s) const noexcept { return nodes_[s].pins; }

    void pin(Slot s) noexcept {
        if (nodes_[s].pins++ == 0) {
            unlink(s);
        }
    }

    void unpin(Slot s) noexcept {
        std::uint32_t& pins = nodes_[s].pins;
        if (pins == 0) {
            return;
        }
        --pins;
        if (pins == 0) {
            link_front(s);
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }
    [[nodiscard]] std::uint64_t evictions() const noexcept { return evictions_; }

private:
    static constexpr Slot kNone = std::numeric_limits<Slot>::max();

    struct Node {
        Key key;
        Value value;
        std::uint32_t pins = 0;
        // Neighbours in the list of unpinned entries, most recently used first.
        Slot prev = kNone;
        Slot next = kNone;
    };

    [[nodiscard]] std::size_t home(const Key& key) const noexcept {
        return static_cast<std::size_t>(hash_(key)) & mask_;
    }

    void link_front(Slot s) noexcept {
        nodes_[s].prev = kNone;
        nodes_[s].next = idle_head_;
        if (idle_head_ != kNone) {
            nodes_[idle_head_].prev = s;
        }
        idle_head_ = s;
        if (idle_tail_ == kNone) {
            idle_tail_ = s;
        }
    }

    void unlink(Slot s) noexcept {
        Node& n = nodes_[s];
        if (n.prev != kNone) {
            nodes_[n.prev].next = n.next;
        } else if (idle_head_ == s) {
            idle_head_ = n.next;
        }
        if (n.next != kNone) {
            nodes_[n.next].prev = n.prev;
        } else if (idle_tail_ == s) {
            idle_tail_ = n.prev;
        }
        n.prev = kNone;
        n.next = kNone;
    }

    void erase_index(Slot s) noexcept {
        std::size_t hole = home(nodes_[s].key);
        while (index_[hole] != s) {
            hole = (hole + 1) & mask_;
        }
        index_[hole] = kNone;
        // Pull back every later entry of the run that the hole would otherwise cut off from
        // its home position.
        for (std::size_t next = (hole + 1) & mask_; index_[next] != kNone;
             next = (next + 1) & mask_) {
            const std::size_t want = home(nodes_[index_[next]].key);
            const bool reachable =
                hole <= next ? (want > hole && want <= next) : (want > hole || want <= next);
            if (!reachable) {
                index_[hole] = index_[next];
                index_[next] = kNone;
                hole = next;
            }
        }
    }

    Hash hash_;
    std::size_t capacity_;
    std::vector<Node> nodes_;
    std::vector<Slot> index_;
    std::size_t mask_;
    Slot idle_head_ = kNone;
    Slot idle_tail_ = kNone;
    std::uint64_t evictions_ = 0;
};

} // namespace http
