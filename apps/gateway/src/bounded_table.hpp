#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace gateway {

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
        : hash_(std::move(hash)), index_(std::bit_ceil(capacity * 2), kNone),
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
        if (nodes_.size() < nodes_.capacity()) {
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
    std::vector<Node> nodes_;
    std::vector<Slot> index_;
    std::size_t mask_;
    Slot idle_head_ = kNone;
    Slot idle_tail_ = kNone;
    std::uint64_t evictions_ = 0;
};

} // namespace gateway
