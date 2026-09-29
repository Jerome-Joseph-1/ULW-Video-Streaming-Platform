#pragma once

#include "core/models/ids.hpp"

#include <optional>
#include <string_view>

namespace chat {

// A live stream's name as the live packager takes it (apps/live-packager/src/stream_id.hpp):
// 1 to 64 of [A-Za-z0-9_-].
[[nodiscard]] bool is_stream_name(std::string_view text) noexcept;

// The room of a stream's live chat: the first 16 bytes of SHA-256 over "ulw-live-chat:" and
// the name, as an RFC 9562 version 8 UUID (core::ports::is_stream_chat, and live_chat_room() in
// SQL). Every node, and any client or service that knows the stream, finds the same room, with
// no table to look it up in. `stream` must be a stream name. nullopt when the digest could not
// be made, which OpenSSL reports only when it cannot allocate.
[[nodiscard]] std::optional<core::RoomId> live_chat_room(std::string_view stream);

} // namespace chat
