#pragma once

#include "core/models/ids.hpp"
#include "core/util/time.hpp"
#include "rt/message_key.hpp"
#include "rt/room_router.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// The JSON a client and chat_server exchange in WebSocket text frames (ADR-0036, ADR-0039).
// Client to server:
//   {"type":"join","room":"<uuid>"}
//       "after":<seq>        optional: also send what this node still holds after that seq
//       "delivery":"lossy"   optional: skip messages while this connection is behind, rather
//                            than be closed for it ("durable", the default)
//   {"type":"send","room":"<uuid>","id":"<message id>","body":"<base64url>"}
// Server to client:
//   {"type":"joined","room":"<uuid>","seq":<integer>}   the room's latest seq known: a client
//                                                      whose last seq is lower missed messages
//   {"type":"sent","room":"<uuid>","id":"<message id>","seq":<integer>}
//   {"type":"message","room":"<uuid>","seq":<integer>,"sender":"<sub>","id":"<message id>",
//    "body":"<base64url>"}
//   {"type":"error","reason":"<code>"}          with "room" and "id" when known, and
//                                               "retry_after_ms" when the reason is rate_limited
// A message id is 1 to 64 of [A-Za-z0-9_-], chosen by the sender and unique per room: sending
// the same id again is answered with the seq the first send got, and delivered once. A body is
// any bytes, in base64url without padding (RFC 4648 section 5); they are carried and returned
// as they came, never read, never logged. Seqs of a room rise by one per message; a jump means
// messages this connection did not get (skipped as lossy, missed while away, or sequenced
// while the room was changing owners), which the client fetches from history.
namespace chat {

enum class Delivery : std::uint8_t { Durable, Lossy };

struct Join {
    core::RoomId room;
    std::optional<std::uint64_t> after;
    Delivery delivery = Delivery::Durable;
};

struct Send {
    core::RoomId room;
    rt::MessageKey id;
    std::vector<std::byte> body;
};

using Command = std::variant<Join, Send>;

enum class EnvelopeError : std::uint8_t {
    NotJson,
    // Not an object, an unknown or missing type, a missing field, a field nobody defined, or a
    // value of the wrong kind.
    Malformed,
    // Not a canonical lowercase UUID.
    BadRoom,
    // Not a message id.
    BadId,
    // Not base64url.
    BadBody,
};

[[nodiscard]] std::expected<Command, EnvelopeError> parse_command(std::string_view text);

// Each appends one message to `out`.
void write_joined(std::string& out, const core::RoomId& room, std::uint64_t head);
void write_sent(std::string& out, const core::RoomId& room, const rt::MessageKey& id,
                std::uint64_t seq);
void write_message(std::string& out, const rt::Message& message);
void write_error(std::string& out, std::string_view reason,
                 const std::optional<core::RoomId>& room = std::nullopt,
                 const std::optional<rt::MessageKey>& id = std::nullopt);
// At most how many bytes a message with a body of `body` bytes takes on the client's socket.
[[nodiscard]] std::size_t message_wire_size(std::size_t body) noexcept;

void write_rate_limited(std::string& out, const core::RoomId& room, const rt::MessageKey& id,
                        core::Millis retry_after);

// The error codes clients see.
[[nodiscard]] std::string_view reason(EnvelopeError e) noexcept;
[[nodiscard]] std::string_view reason(rt::RouteError e) noexcept;

} // namespace chat
