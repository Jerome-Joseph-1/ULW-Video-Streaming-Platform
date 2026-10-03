#pragma once

#include "core/models/ids.hpp"
#include "core/ports/media.hpp"
#include "core/ports/message_store.hpp"
#include "core/util/time.hpp"
#include "rt/message_key.hpp"
#include "rt/room_router.hpp"

#include "ring.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// The JSON a client and chat_server exchange in WebSocket text frames (ADR-0036, ADR-0043).
// Client to server:
//   {"type":"join","room":"<uuid>"}   or, for a live stream's chat, "stream":"<name>" instead
//       of "room": the stream's name as its playback URL carries it. Joined names the room, whose
//       id is what sends to it carry. A stream's chat is always lossy.
//       "after":<seq>        optional: also send what this node still holds after that seq
//       "delivery":"lossy"   optional: skip messages while this connection is behind, rather
//                            than be closed for it ("durable", the default)
//       "kind":"direct"      optional: what the room is. "direct" or "group" (the default)
//                            admit only members, and the first join of a room with no kind
//                            recorded records it. A stream's live chat, which admits anyone
//                            once the server has opened it, is joined by "stream" alone, and
//                            is refused with not_live until then
//   {"type":"send","room":"<uuid>","id":"<message id>","body":"<base64url>"}
//   {"type":"history","room":"<uuid>"}   the room's stored messages, from the store, not this
//       "before":<seq>       optional: those below it, newest first (the default: the newest)
//       "after":<seq>        optional, instead of before: those above it, oldest first
//       "limit":<n>          optional: 1 to 100, 50 when absent
//   {"type":"watch","user":"<sub>"}     hear when the user comes online or goes offline
//   {"type":"unwatch","user":"<sub>"}   stop; unanswered
//   {"type":"call","room":"<uuid>","device":"<uuid>"}   a ticket to the room's call, for this
//       device: the room must be a direct chat this connection has joined (ADR-0050). The first
//       rings the other member; the other member's answers the ring (ADR-0091)
//   {"type":"call_decline"|"call_cancel"|"call_end","room":"<uuid>","call":"<uuid>"}   turn a
//       ringing call down (a callee), give up ringing (the caller), or end an answered call
//       (either member); answered with the event the other member hears
// Member lists (ADR-0096), none of which needs a join first:
//   {"type":"open_direct","user":"<sub>"}   the direct chat with that user, made the first time
//   {"type":"create_group","id":"<request id>","users":["<sub>",...]}   a group chat, the asker
//       its admin; "users" optional, at most 50. The same id again names the same room
//   {"type":"add_members","room":"<uuid>","users":["<sub>",...]}   1 to 50, by an admin
//   {"type":"remove_member","room":"<uuid>","user":"<sub>"}   by an admin
//   {"type":"leave","room":"<uuid>"}   off a group chat's list
//   {"type":"rooms"}   the rooms the user is listed in; "after":"<uuid>", "limit":<1 to 100>
//   {"type":"members","room":"<uuid>"}   a room's members, for a member; "after":"<sub>",
//       "limit":<1 to 100>
// Server to client:
//   {"type":"joined","room":"<uuid>","seq":<integer>}   the room's latest seq known: a client
//                                                      whose last seq is lower missed messages
//   {"type":"sent","room":"<uuid>","id":"<message id>","seq":<integer>}
//   {"type":"message","room":"<uuid>","seq":<integer>,"sender":"<sub>","id":"<message id>",
//    "body":"<base64url>"}
//   {"type":"history","room":"<uuid>","count":<n>}   ends a history answer, after its n
//                                                   messages; 0 when there are no more
//   {"type":"watching","user":"<sub>","status":"online"|"offline"}   the answer to watch: what
//                                                                   this node knows now
//   {"type":"presence","user":"<sub>","status":"online"|"offline"}   each change after that
//   {"type":"ticket","room":"<uuid>","url":"<wss url>","token":"<jwt>","expires_at":<unix s>,
//    "call":"<uuid>"}   the answer to call: connect the SFU's client SDK to url with token before
//       expires_at. call names the ring the ticket belongs to; absent when there is none
//   {"type":"call_ringing","room":"<uuid>","call":"<uuid>","from":"<sub>","expires_at":<unix s>}
//       unasked, on every socket of both members: from is calling, until expires_at
//   {"type":"call_answered"|"call_declined"|"call_cancelled"|"call_ended","room":"<uuid>",
//    "call":"<uuid>","from":"<sub>","by":"<sub>"}   unasked, on every socket of both members
//   {"type":"call_missed","room":"<uuid>","call":"<uuid>","from":"<sub>"}   nobody answered
//   {"type":"direct","room":"<uuid>","user":"<sub>"}   the answer to open_direct
//   {"type":"group","room":"<uuid>","id":"<request id>"}   the answer to create_group
//   {"type":"added","room":"<uuid>","users":[...]}   who add_members listed, not who already was
//   {"type":"removed","room":"<uuid>","user":"<sub>"}   the answer to remove_member
//   {"type":"left","room":"<uuid>"}   the answer to leave; "promoted":"<sub>" when the group's
//       last admin left and that member became its admin
//   {"type":"rooms","rooms":[{"room":..,"kind":"direct"|"group"|"live","role":"member"|"admin",
//    "peer":"<sub>"}],"more":<bool>}   a page; "peer" for a direct chat's other member
//   {"type":"members","room":"<uuid>","members":[{"user":..,"role":..}],"more":<bool>}
//   {"type":"member","room":"<uuid>","user":"<sub>",
//    "change":"added"|"removed"|"promoted"|"demoted"}   unasked: a
//       member list this connection's user is on, or a room it has joined, changed
//   {"type":"error","reason":"<code>"}          with "room" and "id" when known, "user" for a
//                                               watch, and "retry_after_ms" when the reason is
//                                               rate_limited, or unavailable for a call
// A message id is 1 to 64 of [A-Za-z0-9_-], chosen by the sender and unique per room: sending
// the same id again is answered with the seq the first send got, and delivered once. A body is
// any bytes, in base64url without padding (RFC 4648 section 5); they are carried and returned
// as they came, never read, never logged. Seqs of a room rise by one per message; a jump means
// messages this connection did not get (skipped as lossy, missed while away, or sequenced
// while the room was changing owners), which the client fetches from history. Room ids of UUID
// version 8 whose first byte is 0x02 are presence rooms (presence_room.hpp), which no client
// joins or sends to.
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

struct Watch {
    core::UserId user;
};

struct Unwatch {
    core::UserId user;
};

struct Call {
    core::RoomId room;
    core::DeviceId device;
};

// call_decline, call_cancel or call_end.
struct CallMove {
    core::RoomId room;
    CallSignal signal = CallSignal::Decline;
    CallId call;
};

// A page of rooms or of members (core::ports::kMaxListPage at most).
inline constexpr std::size_t kMaxListLimit = 100;
inline constexpr std::size_t kDefaultListLimit = 50;

struct OpenDirect {
    core::UserId user;
};

struct CreateGroup {
    // The client's id for the request, as a message's: a repeat names the same room.
    rt::MessageKey id;
    // Besides the asker; at most core::ports::kMaxMembersPerChange, as given.
    std::vector<core::UserId> users;
};

struct AddMembers {
    core::RoomId room;
    // 1 to core::ports::kMaxMembersPerChange, as given.
    std::vector<core::UserId> users;
};

struct RemoveMember {
    core::RoomId room;
    core::UserId user;
};

struct LeaveRoom {
    core::RoomId room;
};

struct ListRooms {
    std::optional<core::RoomId> after;
    std::size_t limit = kDefaultListLimit;
};

struct ListMembers {
    core::RoomId room;
    std::optional<core::UserId> after;
    std::size_t limit = kDefaultListLimit;
};

using Command =
    std::variant<Join, Send, History, Watch, Unwatch, Call, CallMove, OpenDirect, CreateGroup,
                 AddMembers, RemoveMember, LeaveRoom, ListRooms, ListMembers>;

enum class EnvelopeError : std::uint8_t {
    NotJson,
    // Not an object, an unknown or missing type, a missing field, a field nobody defined, or a
    // value of the wrong kind.
    Malformed,
    // Not a canonical lowercase UUID, or a presence room's.
    BadRoom,
    // Not a message id.
    BadId,
    // Not base64url.
    BadBody,
    // Not a live stream's name.
    BadStream,
    // The command could not be read for want of memory; it may be sent again.
    Unavailable,
    // Not a user id.
    BadUser,
    // Not a device id: a canonical lowercase UUID.
    BadDevice,
    // Not a call id: a canonical lowercase UUID.
    BadCall,
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

// `type` is "watching" or "presence".
void write_presence(std::string& out, std::string_view type, const core::UserId& user, bool online);
void write_user_error(std::string& out, std::string_view reason, const core::UserId& user);

void write_rate_limited(std::string& out, const core::RoomId& room, const rt::MessageKey& id,
                        core::Millis retry_after);

// The answer to a call: the ticket the client takes to the SFU, and the ring it belongs to.
void write_ticket(std::string& out, const core::RoomId& room,
                  const core::ports::MediaTicket& ticket,
                  const std::optional<CallId>& call = std::nullopt);
// What a ring's notice tells each socket of its member, and what a member's own decline,
// cancel or end is answered with.
void write_call_event(std::string& out, const CallNotice& notice);
// A call refused, with a hint of when to ask again for a refusal a retry may cure.
void write_call_error(std::string& out, std::string_view reason, const core::RoomId& room,
                      std::optional<core::Millis> retry_after);

// The answers to the member-list commands (ADR-0096).
void write_direct(std::string& out, const core::RoomId& room, const core::UserId& user);
void write_group(std::string& out, const core::RoomId& room, const rt::MessageKey& id);
void write_added(std::string& out, const core::RoomId& room, std::span<const core::UserId> users);
void write_removed(std::string& out, const core::RoomId& room, const core::UserId& user);
// `promoted`: who became the group's admin because the leaver was its last.
void write_left(std::string& out, const core::RoomId& room,
                const std::optional<core::UserId>& promoted);
void write_rooms(std::string& out, std::span<const core::ports::RoomEntry> rooms, bool more);
void write_members(std::string& out, const core::RoomId& room,
                   std::span<const core::ports::MemberEntry> members, bool more);
// Unasked: `user` was listed in the room's member list ("added"), taken off it ("removed"), or
// made its admin or a plain member again ("promoted", "demoted").
void write_member_change(std::string& out, const core::RoomId& room, const core::UserId& user,
                         std::string_view change);
// A member-list command refused: whichever of the room, the request's id and the user named are
// known, and when to ask again for rate_limited.
struct ErrorContext {
    std::optional<core::RoomId> room;
    std::optional<rt::MessageKey> id;
    std::optional<core::UserId> user;
    std::optional<core::Millis> retry_after;
};
void write_error_with(std::string& out, std::string_view reason, const ErrorContext& context);

// The error codes clients see.
[[nodiscard]] std::string_view reason(EnvelopeError e) noexcept;
[[nodiscard]] std::string_view reason(rt::RouteError e) noexcept;

} // namespace chat
