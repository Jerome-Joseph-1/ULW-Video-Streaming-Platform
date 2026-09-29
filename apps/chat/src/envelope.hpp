#pragma once

#include "core/models/ids.hpp"
#include "rt/room_router.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

// The JSON a client and chat_server exchange in WebSocket text frames, as far as M16 needs it
// (ADR-0034). Client to server:
//   {"type":"join","room":"<uuid>"}
//   {"type":"send","room":"<uuid>","body":"<text>"}          "ref":<integer> is optional
// Server to client:
//   {"type":"joined","room":"<uuid>"}
//   {"type":"sent","room":"<uuid>","ref":<integer>,"seq":<integer>}
//   {"type":"message","room":"<uuid>","seq":<integer>,"sender":"<sub>","body":"<text>"}
//   {"type":"error","reason":"<code>"}                    with "room" and "ref" when known
// A body is opaque: carried and returned as it came, never read, never logged.
namespace chat {

struct Join {
    core::RoomId room;
};

struct Send {
    core::RoomId room;
    // Echoed in the answer, so a client can tell its sends apart.
    std::optional<std::uint64_t> ref;
    std::string body;
};

using Command = std::variant<Join, Send>;

enum class EnvelopeError : std::uint8_t {
    NotJson,
    // Not an object, an unknown or missing type, a missing field, or a field nobody defined.
    Malformed,
    // Not a canonical lowercase UUID.
    BadRoom,
};

[[nodiscard]] std::expected<Command, EnvelopeError> parse_command(std::string_view text);

// Each appends one message to `out`.
void write_joined(std::string& out, const core::RoomId& room);
void write_sent(std::string& out, const core::RoomId& room, std::optional<std::uint64_t> ref,
                std::uint64_t seq);
void write_message(std::string& out, const rt::Message& message);
void write_error(std::string& out, std::string_view reason,
                 const std::optional<core::RoomId>& room = std::nullopt,
                 std::optional<std::uint64_t> ref = std::nullopt);

// The error codes clients see.
[[nodiscard]] std::string_view reason(EnvelopeError e) noexcept;
[[nodiscard]] std::string_view reason(rt::RouteError e) noexcept;

} // namespace chat
