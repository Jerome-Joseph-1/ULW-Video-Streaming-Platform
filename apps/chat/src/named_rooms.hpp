#pragma once

#include "core/models/ids.hpp"
#include "rt/message_key.hpp"

namespace chat {

// Rooms the service names from what they are for (ADR-0096), as it names a user's presence room
// (presence_room.hpp) and a stream's chat (live_chat.hpp): RFC 9562 version 8 UUIDs, name-based
// on SHA-256, whose first byte is the core::ports::NamedRoom tag of their kind. Every node
// derives the same room from the same name with nothing looked up, so opening one twice, on any
// node, names one room.

// The direct chat of two users, whichever of them asks: the pair is hashed in byte order of the
// ids. The two may be the same user only in the hash; nothing lists a direct chat with oneself.
[[nodiscard]] core::RoomId direct_room(const core::UserId& a, const core::UserId& b);

// The group chat a creator's request made: `request` is the id the client gave the create, so a
// create repeated after a lost answer names the room the first one made.
[[nodiscard]] core::RoomId group_room(const core::UserId& creator, const rt::MessageKey& request);

} // namespace chat
