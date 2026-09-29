#include "envelope.hpp"

#include "core/util/json.hpp"
#include "infra/auth/base64url.hpp"

#include <algorithm>
#include <array>
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
    if (!id) {
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

std::expected<Command, EnvelopeError> join_of(const core::json::Value& message) {
    if (!only(message, {"type", "room", "after", "delivery"})) {
        return std::unexpected(EnvelopeError::Malformed);
    }
    auto room = room_of(message);
    if (!room) {
        return std::unexpected(room.error());
    }
    Join join{.room = *room, .after = std::nullopt, .delivery = Delivery::Durable};
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

std::expected<Command, EnvelopeError> history_of(const core::json::Value& message) {
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
    out += infra::auth::encode_base64url(octets);
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

void write_rate_limited(std::string& out, const core::RoomId& room, const rt::MessageKey& id,
                        core::Millis retry_after) {
    write_error(out, "rate_limited", room, id);
    out.pop_back();
    std::format_to(std::back_inserter(out), R"(,"retry_after_ms":{}}})", retry_after.count());
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
