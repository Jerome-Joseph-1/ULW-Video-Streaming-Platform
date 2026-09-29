#pragma once

#include "core/models/ids.hpp"
#include "core/util/time.hpp"
#include "rt/message_key.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>

namespace rt {

// The keys of messages this node sequenced or delivered lately, with the seq each got. A send
// whose key is here was sequenced already: it is answered with that seq and not sequenced
// again. Bounded in entries and in age, whichever bites first; the oldest go first.
class RecentKeys {
public:
    RecentKeys(std::size_t capacity, core::Millis window) noexcept
        : capacity_(capacity), window_(window) {}

    [[nodiscard]] std::optional<std::uint64_t>
    find(const core::RoomId& room, const core::UserId& sender, const MessageKey& key) const;
    // The first seq remembered for a key stays: a delivery repeated by a resubscription is the
    // same message.
    void remember(const core::RoomId& room, const core::UserId& sender, const MessageKey& key,
                  std::uint64_t seq, core::MonoTime now);
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
    std::unordered_map<Entry, std::uint64_t, EntryHash> seqs_;
    std::deque<Remembered> order_;
};

} // namespace rt
