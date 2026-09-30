#pragma once

#include "core/models/ids.hpp"
#include "rt/room_store.hpp"

namespace chat {

// The room a user's presence travels in: an RFC 9562 version 8 UUID, name-based on SHA-256 as
// the RFC's own example is, so every node derives the same room for a user with nothing looked
// up. Its first byte is core::ports::NamedRoom::Presence, never a stream chat's tag. A version 8 id
// with that tag is the room plane's ephemeral kind (rt::is_ephemeral_room): its seqs are taken and
// no message is stored, and clients cannot join, send to or read such a room as a chat room.
[[nodiscard]] core::RoomId presence_room(const core::UserId& user);

[[nodiscard]] inline bool is_presence_room(const core::RoomId& room) noexcept {
    return rt::is_ephemeral_room(room);
}

} // namespace chat
