#include "envelope.hpp"

#include "core/util/json.hpp"
#include "infra/auth/base64url.hpp"

#include "live_chat.hpp"
#include "presence_room.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <iterator>
#include <span>

namespace chat {

namespace {

std::expected<core::RoomId, EnvelopeError> room_of(const core::json::Value& message) {
    const core::json::Value* room = message.find("room");
    const auto text = room == nullptr ? std::nullopt : room->as_string();
    if (!text) {
        return std::unexpected(EnvelopeError::Malformed);
    }
    const auto id = core::RoomId::parse(*text);
    if (!id || is_presence_room(*id)) {
        return std::unexpected(EnvelopeError::BadRoom);
    }
    return *id;
}

// Only the members a command defines: a misspelt optional field is refused rather than
// silently ignored.
bool only(const core::json::Value& message, std::initializer_list<std::string_view> allowed) {
    const auto* members = message.as_object();
    return std::ranges::all_of(*members, [&](const core::json::Value::Member& m) {
        return std::ranges::find(allowed, m.first) != allowed.end();
    });
}

std::optional<std::string_view> string_of(const core::json::Value& message, std::string_view key) {
    const core::json::Value* v = message.find(key);
    return v == nullptr ? std::nullopt : v->as_string();
}

// A stream's live chat is joined by the stream's name, never by its room's id: the name is what
// makes the room a live chat, and a live chat is the only room an id of its kind names.
std::expected<core::RoomId, EnvelopeError> joined_room_of(const core::json::Value& message) {
    if (message.find("stream") == nullptr) {
        auto room = room_of(message);
        if (room && core::ports::is_stream_chat(*room)) {
            return std::unexpected(EnvelopeError::BadRoom);
        }
        return room;
    }
    const auto stream = string_of(message, "stream");
    if (message.find("room") != nullptr || message.find("kind") != nullptr || !stream) {
        return std::unexpected(EnvelopeError::Malformed);
    }
    if (!is_stream_name(*stream)) {
        return std::unexpected(EnvelopeError::BadStream);
    }
    const auto room = live_chat_room(*stream);
    if (!room) {
        return std::unexpected(EnvelopeError::Unavailable);
    }
    return *room;
}

std::expected<Command, EnvelopeError> join_of(const core::json::Value& message) {
    if (!only(message, {"type", "room", "stream", "after", "delivery", "kind"})) {
        return std::unexpected(EnvelopeError::Malformed);
    }
    auto room = joined_room_of(message);
    if (!room) {
        return std::unexpected(room.error());
    }
    Join join{.room = *room,
              .after = std::nullopt,
              .delivery = Delivery::Durable,
              .kind = core::ports::is_stream_chat(*room) ? core::ports::RoomKind::StreamLiveChat
                                                         : core::ports::RoomKind::GroupChat};
    if (const core::json::Value* after = message.find("after")) {
        join.after = after->as_u64();
        if (!join.after) {
            return std::unexpected(EnvelopeError::Malformed);
        }
    }
    if (message.find("delivery") != nullptr) {
        const auto delivery = string_of(message, "delivery");
        if (delivery == "lossy") {
            join.delivery = Delivery::Lossy;
        } else if (delivery != "durable") {
            return std::unexpected(EnvelopeError::Malformed);
        }
    }
    if (message.find("kind") != nullptr) {
        const auto kind = string_of(message, "kind");
        if (kind == "direct") {
            join.kind = core::ports::RoomKind::DirectChat;
        } else if (kind != "group") {
            return std::unexpected(EnvelopeError::Malformed);
        }
    }
    return join;
}

std::expected<Command, EnvelopeError> send_of(const core::json::Value& message) {
    if (!only(message, {"type", "room", "id", "body"})) {
        return std::unexpected(EnvelopeError::Malformed);
    }
    auto room = room_of(message);
    if (!room) {
        return std::unexpected(room.error());
    }
    const auto id_text = string_of(message, "id");
    const auto body_text = string_of(message, "body");
    if (!id_text || !body_text) {
        return std::unexpected(EnvelopeError::Malformed);
    }
    const auto id = rt::MessageKey::parse(*id_text);
    if (!id) {
        return std::unexpected(EnvelopeError::BadId);
    }
    // Transfer encoding only: what the bytes are is the clients' business.
    auto body = infra::auth::decode_base64url_bytes(*body_text);
    if (!body) {
        return std::unexpected(EnvelopeError::BadBody);
    }
    return Send{.room = *room, .id = *id, .body = std::move(*body)};
}

[[nodiscard]] std::expected<Command, EnvelopeError> history_of(const core::json::Value& message) {
    if (!only(message, {"type", "room", "before", "after", "limit"})) {
        return std::unexpected(EnvelopeError::Malformed);
    }
    auto room = room_of(message);
    if (!room) {
        return std::unexpected(room.error());
    }
    History history{.room = *room, .before = std::nullopt, .after = std::nullopt};
    const auto seq_at = [&](std::string_view field, std::optional<std::uint64_t>& into) {
        const core::json::Value* v = message.find(field);
        if (v == nullptr) {
            return true;
        }
        into = v->as_u64();
        return into.has_value();
    };
    if (!seq_at("before", history.before) || !seq_at("after", history.after) ||
        (history.before && history.after)) {
        return std::unexpected(EnvelopeError::Malformed);
    }
    if (const core::json::Value* limit = message.find("limit")) {
        const auto n = limit->as_u64();
        if (!n || *n == 0 || *n > kMaxHistoryLimit) {
            return std::unexpected(EnvelopeError::Malformed);
        }
        history.limit = static_cast<std::size_t>(*n);
    }
    return history;
}

std::expected<core::UserId, EnvelopeError> user_of(const core::json::Value& message) {
    if (!only(message, {"type", "user"})) {
        return std::unexpected(EnvelopeError::Malformed);
    }
    const auto text = string_of(message, "user");
    if (!text) {
        return std::unexpected(EnvelopeError::Malformed);
    }
    const auto user = core::UserId::parse(*text);
    if (!user) {
        return std::unexpected(EnvelopeError::BadUser);
    }
    return *user;
}

std::expected<Command, EnvelopeError> call_of(const core::json::Value& message) {
    if (!only(message, {"type", "room", "device", "answer"})) {
        return std::unexpected(EnvelopeError::Malformed);
    }
    auto room = room_of(message);
    if (!room) {
        return std::unexpected(room.error());
    }
    const auto text = string_of(message, "device");
    if (!text) {
        return std::unexpected(EnvelopeError::Malformed);
    }
    const auto device = core::DeviceId::parse(*text);
    if (!device) {
        return std::unexpected(EnvelopeError::BadDevice);
    }
    std::optional<CallId> answering;
    if (message.find("answer") != nullptr) {
        const auto named = string_of(message, "answer");
        if (!named) {
            return std::unexpected(EnvelopeError::Malformed);
        }
        const auto parsed = CallId::parse(*named);
        if (!parsed) {
            return std::unexpected(EnvelopeError::BadCall);
        }
        answering = *parsed;
    }
    return Call{.room = *room, .device = *device, .answering = answering};
}

std::expected<Command, EnvelopeError> call_move_of(const core::json::Value& message,
                                                   CallSignal signal) {
    const bool expel = signal == CallSignal::Expel;
    if (expel ? !only(message, {"type", "room", "call", "user"})
              : !only(message, {"type", "room", "call"})) {
        return std::unexpected(EnvelopeError::Malformed);
    }
    auto room = room_of(message);
    if (!room) {
        return std::unexpected(room.error());
    }
    const auto text = string_of(message, "call");
    if (!text) {
        return std::unexpected(EnvelopeError::Malformed);
    }
    const auto call = CallId::parse(*text);
    if (!call) {
        return std::unexpected(EnvelopeError::BadCall);
    }
    if (!expel) {
        return CallMove{.room = *room, .signal = signal, .call = *call};
    }
    const auto named = string_of(message, "user");
    if (!named) {
        return std::unexpected(EnvelopeError::Malformed);
    }
    const auto target = core::UserId::parse(*named);
    if (!target) {
        return std::unexpected(EnvelopeError::BadUser);
    }
    return CallMove{.room = *room, .signal = signal, .call = *call, .target = *target};
}

void append_room(std::string& out, const core::RoomId& room) {
    std::array<char, core::Uuid::kTextLength> text{};
    room.format_to(text);
    out += R"("room":")";
    out.append(text.data(), text.size());
    out += '"';
}

// Message ids hold only characters that need no escaping.
void append_id(std::string& out, const rt::MessageKey& id) {
    out += R"(,"id":")";
    out += id.view();
    out += '"';
}

} // namespace

std::expected<Command, EnvelopeError> parse_command(std::string_view text) {
    const auto message = core::json::parse(text);
    if (!message) {
        return std::unexpected(EnvelopeError::NotJson);
    }
    if (message->as_object() == nullptr) {
        return std::unexpected(EnvelopeError::Malformed);
    }
    const auto name = string_of(*message, "type");
    if (name == "join") {
        return join_of(*message);
    }
    if (name == "send") {
        return send_of(*message);
    }
    if (name == "history") {
        return history_of(*message);
    }
    if (name == "call") {
        return call_of(*message);
    }
    if (name == "call_decline") {
        return call_move_of(*message, CallSignal::Decline);
    }
    if (name == "call_cancel") {
        return call_move_of(*message, CallSignal::Cancel);
    }
    if (name == "call_end") {
        return call_move_of(*message, CallSignal::End);
    }
    if (name == "call_leave") {
        return call_move_of(*message, CallSignal::Leave);
    }
    if (name == "call_expel") {
        return call_move_of(*message, CallSignal::Expel);
    }
    if (name == "watch" || name == "unwatch") {
        const auto user = user_of(*message);
        if (!user) {
            return std::unexpected(user.error());
        }
        if (name == "watch") {
            return Watch{.user = *user};
        }
        return Unwatch{.user = *user};
    }
    return std::unexpected(EnvelopeError::Malformed);
}

void write_joined(std::string& out, const core::RoomId& room, std::uint64_t head) {
    out += R"({"type":"joined",)";
    append_room(out, room);
    std::format_to(std::back_inserter(out), R"(,"seq":{}}})", head);
}

void write_sent(std::string& out, const core::RoomId& room, const rt::MessageKey& id,
                std::uint64_t seq) {
    out += R"({"type":"sent",)";
    append_room(out, room);
    append_id(out, id);
    std::format_to(std::back_inserter(out), R"(,"seq":{}}})", seq);
}

void write_message(std::string& out, const rt::Message& message) {
    // Grown once, to its final size: a message is up to tens of KiB, and a string doubling its
    // way there leaves every smaller copy behind as a hole in the heap.
    out.reserve(out.size() + message_wire_size(message.body.size()));
    out += R"({"type":"message",)";
    append_room(out, message.room);
    std::format_to(std::back_inserter(out), R"(,"seq":{},"sender":)", message.seq);
    // UserId allows only characters that need no escaping.
    core::json::append_string(out, message.sender.view());
    append_id(out, message.key);
    out += R"(,"body":")";
    // The body's bytes, whatever they are; the encoding reads them as octets.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    const std::span<const unsigned char> octets{
        reinterpret_cast<const unsigned char*>(message.body.data()), message.body.size()};
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    infra::auth::append_base64url(out, octets);
    out += R"("})";
}

void write_history(std::string& out, const core::RoomId& room, std::size_t count) {
    out += R"({"type":"history",)";
    append_room(out, room);
    std::format_to(std::back_inserter(out), R"(,"count":{}}})", count);
}

void write_error(std::string& out, std::string_view reason, const std::optional<core::RoomId>& room,
                 const std::optional<rt::MessageKey>& id) {
    out += R"({"type":"error","reason":)";
    core::json::append_string(out, reason);
    if (room) {
        out += ',';
        append_room(out, *room);
    }
    if (id) {
        append_id(out, *id);
    }
    out += '}';
}

// base64url without padding is four characters per three bytes and two or three for a partial
// group. Around it, {"type":"message", the room (45), the seq (27), the sender (at most 140),
// the id (at most 72) and "body":"" (11) come to 313 bytes, and the WebSocket header of a frame
// under 64 KiB to 4 more.
std::size_t message_wire_size(std::size_t body) noexcept {
    constexpr std::size_t kAround = 320;
    return (((body * 4) + 2) / 3) + kAround;
}

void write_presence(std::string& out, std::string_view type, const core::UserId& user,
                    bool online) {
    out += R"({"type":")";
    out += type;
    out += R"(","user":)";
    core::json::append_string(out, user.view());
    out += online ? R"(,"status":"online"})" : R"(,"status":"offline"})";
}

void write_user_error(std::string& out, std::string_view reason, const core::UserId& user) {
    out += R"({"type":"error","reason":)";
    core::json::append_string(out, reason);
    out += R"(,"user":)";
    core::json::append_string(out, user.view());
    out += '}';
}

void write_rate_limited(std::string& out, const core::RoomId& room, const rt::MessageKey& id,
                        core::Millis retry_after) {
    write_error(out, "rate_limited", room, id);
    out.pop_back();
    std::format_to(std::back_inserter(out), R"(,"retry_after_ms":{}}})", retry_after.count());
}

void write_ticket(std::string& out, const core::RoomId& room,
                  const core::ports::MediaTicket& ticket, const std::optional<CallId>& call) {
    out += R"({"type":"ticket",)";
    append_room(out, room);
    out += R"(,"url":)";
    core::json::append_string(out, ticket.endpoint);
    out += R"(,"token":)";
    core::json::append_string(out, ticket.credential);
    std::format_to(
        std::back_inserter(out), R"(,"expires_at":{})",
        std::chrono::duration_cast<core::Seconds>(ticket.expires_at.time_since_epoch()).count());
    if (call) {
        out += R"(,"call":")";
        out += call->to_string();
        out += '"';
    }
    out += '}';
}

void write_call_event(std::string& out, const CallNotice& notice) {
    std::string_view type = "call_ringing";
    switch (notice.event) {
    case RingEvent::Ringing:
        type = "call_ringing";
        break;
    case RingEvent::Answered:
        type = "call_answered";
        break;
    case RingEvent::Declined:
        type = "call_declined";
        break;
    case RingEvent::Cancelled:
        type = "call_cancelled";
        break;
    case RingEvent::Missed:
        type = "call_missed";
        break;
    case RingEvent::Ended:
        type = "call_ended";
        break;
    case RingEvent::Left:
        type = "call_left";
        break;
    case RingEvent::Moved:
        type = "call_moved";
        break;
    }
    out += R"({"type":")";
    out += type;
    out += R"(",)";
    append_room(out, notice.room);
    out += R"(,"call":")";
    out += notice.call.to_string();
    out += R"(","from":)";
    core::json::append_string(out, notice.from.view());
    if (notice.event == RingEvent::Ringing) {
        std::format_to(
            std::back_inserter(out), R"(,"expires_at":{})",
            std::chrono::duration_cast<core::Seconds>(notice.expires_at.time_since_epoch())
                .count());
    } else if (notice.by) {
        out += R"(,"by":)";
        core::json::append_string(out, notice.by->view());
    }
    if (notice.event == RingEvent::Moved && notice.subject) {
        out += R"(,"expelled":)";
        core::json::append_string(out, notice.subject->view());
    }
    out += '}';
}

void write_call_over(std::string& out, const core::RoomId& room, const CallId& call) {
    write_error(out, "no_call", room);
    out.pop_back();
    out += R"(,"call":")";
    out += call.to_string();
    out += R"("})";
}

void write_call_error(std::string& out, std::string_view reason, const core::RoomId& room,
                      std::optional<core::Millis> retry_after) {
    write_error(out, reason, room);
    if (retry_after) {
        out.pop_back();
        std::format_to(std::back_inserter(out), R"(,"retry_after_ms":{}}})", retry_after->count());
    }
}

std::string_view reason(EnvelopeError e) noexcept {
    switch (e) {
    case EnvelopeError::NotJson:
        return "not_json";
    case EnvelopeError::Malformed:
        return "malformed";
    case EnvelopeError::BadRoom:
        return "bad_room";
    case EnvelopeError::BadId:
        return "bad_id";
    case EnvelopeError::BadBody:
        return "bad_body";
    case EnvelopeError::BadStream:
        return "bad_stream";
    case EnvelopeError::Unavailable:
        return "unavailable";
    case EnvelopeError::BadUser:
        return "bad_user";
    case EnvelopeError::BadDevice:
        return "bad_device";
    case EnvelopeError::BadCall:
        return "bad_call";
    }
    return "malformed";
}

std::string_view reason(rt::RouteError e) noexcept {
    switch (e) {
    case rt::RouteError::NotJoined:
        return "not_joined";
    case rt::RouteError::Fenced:
        return "fenced";
    case rt::RouteError::Unavailable:
        return "unavailable";
    case rt::RouteError::Busy:
        return "busy";
    case rt::RouteError::Conflict:
        return "conflict";
    }
    return "unavailable";
}

} // namespace chat
