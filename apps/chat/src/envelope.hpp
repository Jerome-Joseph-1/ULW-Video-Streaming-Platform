#pragma once

#include "core/models/ids.hpp"
#include "core/ports/message_store.hpp"
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

// The JSON a client and chat_server exchange in WebSocket text frames (ADR-0036, ADR-0043).
// Client to server:
//   {"type":"join","room":"<uuid>"}
//       "after":<seq>        optional: also send what this node still holds after that seq
//       "delivery":"lossy"   optional: skip messages while this connection is behind, rather
//                            than be closed for it ("durable", the default)
//       "kind":"live"        optional: what the room is, recorded by the room's first join:
//                            "direct" or "group" (the default) admit only members, "live"
//                            admits anyone
//   {"type":"send","room":"<uuid>","id":"<message id>","body":"<base64url>"}
//   {"type":"history","room":"<uuid>"}   the room's stored messages, from the store, not this
//       "before":<seq>       optional: those below it, newest first (the default: the newest)
//       "after":<seq>        optional, instead of before: those above it, oldest first
//       "limit":<n>          optional: 1 to 100, 50 when absent
// Server to client:
//   {"type":"joined","room":"<uuid>","seq":<integer>}   the room's latest seq known: a client
//                                                      whose last seq is lower missed messages
//   {"type":"sent","room":"<uuid>","id":"<message id>","seq":<integer>}
//   {"type":"message","room":"<uuid>","seq":<integer>,"sender":"<sub>","id":"<message id>",
//    "body":"<base64url>"}
//   {"type":"history","room":"<uuid>","count":<n>}   ends a history answer, after its n
//                                                   messages; 0 when there are no more
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
    core::ports::RoomKind kind = core::ports::RoomKind::GroupChat;
};

struct Send {
    core::RoomId room;
    rt::MessageKey id;
    std::vector<std::byte> body;
};

// A history page is sent to the client as message frames, so it counts against the same output
// the client has not read yet; 100 ordinary messages are tens of kilobytes, and the service cuts
// a page of larger ones short to what the client can take (ServiceLimits::replay_budget).
inline constexpr std::size_t kMaxHistoryLimit = 100;
inline constexpr std::size_t kDefaultHistoryLimit = 50;

struct History {
    core::RoomId room;
    // Neither: the newest messages, newest first.
    std::optional<std::uint64_t> before;
    std::optional<std::uint64_t> after;
    std::size_t limit = kDefaultHistoryLimit;
};

using Command = std::variant<Join, Send, History>;

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
// Ends the answer to a history command, after its `count` messages.
void write_history(std::string& out, const core::RoomId& room, std::size_t count);
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
