#pragma once

#include "core/models/ids.hpp"
#include "core/util/time.hpp"
#include "rt/message_key.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <unordered_map>

namespace rt {

// The keys of messages this node sequenced or delivered lately, with the seq each got and a
// digest of its body. A send whose key is here was sequenced already: with the same body it is
// answered with that seq and not sequenced again; with another it is a conflict, as the store
// would find it. Bounded in entries and in age, whichever bites first; the oldest go first.
class RecentKeys {
public:
    struct Sequenced {
        std::uint64_t seq = 0;
        std::uint64_t digest = 0;
        friend bool operator==(const Sequenced&, const Sequenced&) = default;
    };

    RecentKeys(std::size_t capacity, core::Millis window) noexcept
        : capacity_(capacity), window_(window) {}

    // FNV-1a over the body: tells a repeat from another message under the same key. Only the
    // sender's own messages share a key, so a collision it made would only cost it its own.
    [[nodiscard]] static std::uint64_t digest(std::span<const std::byte> body) noexcept;

    [[nodiscard]] std::optional<Sequenced>
    find(const core::RoomId& room, const core::UserId& sender, const MessageKey& key) const;
    // The first seq remembered for a key stays: a delivery repeated by a resubscription is the
    // same message.
    void remember(const core::RoomId& room, const core::UserId& sender, const MessageKey& key,
                  Sequenced sequenced, core::MonoTime now);
    [[nodiscard]] std::size_t size() const noexcept { return seqs_.size(); }

private:
    struct Entry {
        core::RoomId room;
        core::UserId sender;
        MessageKey key;
        friend bool operator==(const Entry&, const Entry&) = default;
    };
    struct EntryHash {
        [[nodiscard]] std::size_t operator()(const Entry& e) const noexcept;
    };
    // Points at the map's own key, which stays put when the map rehashes.
    struct Remembered {
        const Entry* entry = nullptr;
        core::MonoTime at;
    };

    std::size_t capacity_;
    core::Millis window_;
    std::unordered_map<Entry, Sequenced, EntryHash> seqs_;
    std::deque<Remembered> order_;
};

} // namespace rt
