#pragma once

#include "core/models/ids.hpp"

#include <string_view>

namespace chat {

// A live stream's name as the live packager takes it (apps/live-packager/src/stream_id.hpp):
// 1 to 64 of [A-Za-z0-9_-].
[[nodiscard]] bool is_stream_name(std::string_view text) noexcept;

// The room of a stream's live chat: the first 16 bytes of SHA-256 over "ulw-live-chat:" and
// the name, as an RFC 9562 version 8 UUID. Every node, and any client or service that knows the
// stream, finds the same room, with no table to look it up in. `stream` must be a stream name.
[[nodiscard]] core::RoomId live_chat_room(std::string_view stream);

// Whether the room is some stream's live chat. Nothing else here makes a version 8 id (ours
// are version 7, ADR-0023), and a client cannot join one by its id, only by its stream.
[[nodiscard]] bool is_live_chat(const core::RoomId& room) noexcept;

} // namespace chat
