#include "envelope.hpp"

#include "core/util/json.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <iterator>

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

std::expected<Command, EnvelopeError> send_of(const core::json::Value& message) {
    if (!only(message, {"type", "room", "ref", "body"})) {
        return std::unexpected(EnvelopeError::Malformed);
    }
    auto room = room_of(message);
    if (!room) {
        return std::unexpected(room.error());
    }
    const core::json::Value* body = message.find("body");
    const auto text = body == nullptr ? std::nullopt : body->as_string();
    if (!text) {
        return std::unexpected(EnvelopeError::Malformed);
    }
    std::optional<std::uint64_t> ref;
    if (const core::json::Value* r = message.find("ref")) {
        ref = r->as_u64();
        if (!ref) {
            return std::unexpected(EnvelopeError::Malformed);
        }
    }
    return Send{.room = *room, .ref = ref, .body = std::string(*text)};
}

void append_room(std::string& out, const core::RoomId& room) {
    std::array<char, core::Uuid::kTextLength> text{};
    room.format_to(text);
    out += R"("room":")";
    out.append(text.data(), text.size());
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
    const core::json::Value* type = message->find("type");
    const auto name = type == nullptr ? std::nullopt : type->as_string();
    if (name == "join") {
        if (!only(*message, {"type", "room"})) {
            return std::unexpected(EnvelopeError::Malformed);
        }
        auto room = room_of(*message);
        if (!room) {
            return std::unexpected(room.error());
        }
        return Join{.room = *room};
    }
    if (name == "send") {
        return send_of(*message);
    }
    return std::unexpected(EnvelopeError::Malformed);
}

void write_joined(std::string& out, const core::RoomId& room) {
    out += R"({"type":"joined",)";
    append_room(out, room);
    out += '}';
}

void write_sent(std::string& out, const core::RoomId& room, std::optional<std::uint64_t> ref,
                std::uint64_t seq) {
    out += R"({"type":"sent",)";
    append_room(out, room);
    if (ref) {
        std::format_to(std::back_inserter(out), R"(,"ref":{})", *ref);
    }
    std::format_to(std::back_inserter(out), R"(,"seq":{}}})", seq);
}

void write_message(std::string& out, const rt::Message& message) {
    out += R"({"type":"message",)";
    append_room(out, message.room);
    std::format_to(std::back_inserter(out), R"(,"seq":{},"sender":)", message.seq);
    // UserId allows only characters that need no escaping.
    core::json::append_string(out, message.sender.view());
    out += R"(,"body":)";
    // Valid UTF-8: it came from a JSON string, whose parser refuses anything else. The bytes
    // are characters, which is what reading them as such means.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const std::string_view body{reinterpret_cast<const char*>(message.body.data()),
                                message.body.size()};
    core::json::append_string(out, body);
    out += '}';
}

void write_error(std::string& out, std::string_view reason, const std::optional<core::RoomId>& room,
                 std::optional<std::uint64_t> ref) {
    out += R"({"type":"error","reason":)";
    core::json::append_string(out, reason);
    if (room) {
        out += ',';
        append_room(out, *room);
    }
    if (ref) {
        std::format_to(std::back_inserter(out), R"(,"ref":{})", *ref);
    }
    out += '}';
}

std::string_view reason(EnvelopeError e) noexcept {
    switch (e) {
    case EnvelopeError::NotJson:
        return "not_json";
    case EnvelopeError::Malformed:
        return "malformed";
    case EnvelopeError::BadRoom:
        return "bad_room";
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
    }
    return "unavailable";
}

} // namespace chat
