#pragma once

#include "core/models/ids.hpp"
#include "core/util/time.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

// What the chat service hands the room plane as unsequenced notices (rt::RoomRouter::notify), as
// the ring does (ring.hpp), and the nodes that hear them push to their sockets. Each layout's
// first byte says which it is; the ring's is 1, and a node that reads a byte it does not know
// drops the notice, so a node of an earlier release hears none of these and counts them as
// malformed.
namespace chat {

// A room moved on (ADR-0137): told to one member's presence room, which every node with a socket
// of theirs is in, so that each of those sockets hears it whether it joined the room or not.
struct ActivityNotice {
    core::UserId to;
    core::RoomId room;
    // The latest message this notice covers: its seq, sender and when it was sequenced.
    std::uint64_t seq = 0;
    core::UserId sender;
    // Unix milliseconds on the wire.
    core::WallTime at;
    // Every message since the member's last notice of this room was quiet (ADR-0128).
    bool quiet = false;
};

[[nodiscard]] std::vector<std::byte> encode_activity(const ActivityNotice& notice);
// nullopt for any other layout, or one that does not read whole.
[[nodiscard]] std::optional<ActivityNotice> decode_activity(std::span<const std::byte> bytes);
// Whether the notice is in the activity layout, by its first byte alone.
[[nodiscard]] bool is_activity(std::span<const std::byte> bytes) noexcept;

// Told to the room itself, so that it reaches the room's joined sockets on every node that has
// some (ADR-0138): a member is typing, never stored; or a member's read mark moved, once stored.
struct TypingNotice {
    core::RoomId room;
    core::UserId user;
};

struct ReadNotice {
    core::RoomId room;
    core::UserId user;
    std::uint64_t seq = 0;
};

// Told to the room too, once the store has blanked a message (ADR-0135): every node with members
// in the room blanks what it keeps of it and tells its joined sockets.
struct RedactionNotice {
    core::RoomId room;
    std::uint64_t seq = 0;
};

using RoomNotice = std::variant<TypingNotice, ReadNotice, RedactionNotice>;

[[nodiscard]] std::vector<std::byte> encode_typing(const TypingNotice& notice);
[[nodiscard]] std::vector<std::byte> encode_read(const ReadNotice& notice);
[[nodiscard]] std::vector<std::byte> encode_redaction(const RedactionNotice& notice);
// Any of them, or nullopt for any other layout, or one that does not read whole.
[[nodiscard]] std::optional<RoomNotice> decode_room_notice(std::span<const std::byte> bytes);

} // namespace chat
