#pragma once

#include "core/models/ids.hpp"

namespace chat {

// The room a user's presence travels in: an RFC 9562 version 8 UUID, name-based on SHA-256 as
// the RFC's own example is, so every node derives the same room for a user with nothing looked
// up. Version 8 is reserved to it: clients cannot join or send to such a room as a chat room.
[[nodiscard]] core::RoomId presence_room(const core::UserId& user);
[[nodiscard]] bool is_presence_room(const core::RoomId& room) noexcept;

} // namespace chat
