#include "wire.hpp"

#include <array>
#include <string_view>
#include <utility>

namespace rt::wire {

namespace {

constexpr std::size_t kLengthBytes = 4;
constexpr std::size_t kRoomBytes = core::Uuid::kTextLength;

void put_u8(std::vector<std::byte>& out, std::uint8_t v) {
    out.push_back(static_cast<std::byte>(v));
}

void put_u32(std::vector<std::byte>& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((v >> static_cast<unsigned>(shift)) & 0xFFU));
    }
}

void put_u64(std::vector<std::byte>& out, std::uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((v >> static_cast<unsigned>(shift)) & 0xFFU));
    }
}

void put_text(std::vector<std::byte>& out, std::string_view text) {
    const auto bytes = std::as_bytes(std::span{text});
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void put_room(std::vector<std::byte>& out, const core::RoomId& room) {
    std::array<char, kRoomBytes> text{};
    room.format_to(text);
    put_text(out, {text.data(), text.size()});
}

// Nodes and senders are at most 63 and 128 characters.
void put_short(std::vector<std::byte>& out, std::string_view text) {
    put_u8(out, static_cast<std::uint8_t>(text.size()));
    put_text(out, text);
}

// Reserves the length, writes the type, and returns where the length goes once the frame is
// complete.
std::size_t begin(std::vector<std::byte>& out, Type type) {
    const std::size_t at = out.size();
    put_u32(out, 0);
    put_u8(out, static_cast<std::uint8_t>(type));
    return at;
}

void end(std::vector<std::byte>& out, std::size_t at) {
    const auto length = static_cast<std::uint32_t>(out.size() - at - kLengthBytes);
    for (std::size_t i = 0; i < kLengthBytes; ++i) {
        out[at + i] = static_cast<std::byte>((length >> (8 * (kLengthBytes - 1 - i))) & 0xFFU);
    }
}

class Cursor {
public:
    explicit Cursor(std::span<const std::byte> bytes) noexcept : rest_(bytes) {}

    [[nodiscard]] std::optional<std::span<const std::byte>> take(std::size_t n) noexcept {
        if (rest_.size() < n) {
            return std::nullopt;
        }
        const auto out = rest_.first(n);
        rest_ = rest_.subspan(n);
        return out;
    }

    [[nodiscard]] std::optional<std::uint64_t> u64() noexcept {
        const auto bytes = take(sizeof(std::uint64_t));
        if (!bytes) {
            return std::nullopt;
        }
        std::uint64_t v = 0;
        for (const std::byte b : *bytes) {
            v = (v << 8U) | std::to_integer<std::uint64_t>(b);
        }
        return v;
    }

    [[nodiscard]] std::optional<std::uint8_t> u8() noexcept {
        const auto bytes = take(1);
        if (!bytes) {
            return std::nullopt;
        }
        return std::to_integer<std::uint8_t>(bytes->front());
    }

    [[nodiscard]] std::optional<std::string_view> text(std::size_t n) noexcept {
        const auto bytes = take(n);
        if (!bytes) {
            return std::nullopt;
        }
        // The bytes are characters; reading them as such is what this format means.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        return std::string_view{reinterpret_cast<const char*>(bytes->data()), bytes->size()};
    }

    [[nodiscard]] std::optional<std::string_view> short_text() noexcept {
        const auto n = u8();
        if (!n) {
            return std::nullopt;
        }
        return text(*n);
    }

    [[nodiscard]] std::optional<core::RoomId> room() noexcept {
        const auto t = text(kRoomBytes);
        if (!t) {
            return std::nullopt;
        }
        const auto id = core::RoomId::parse(*t);
        return id ? std::optional(*id) : std::nullopt;
    }

    [[nodiscard]] std::optional<core::UserId> sender() noexcept {
        const auto t = short_text();
        if (!t) {
            return std::nullopt;
        }
        const auto id = core::UserId::parse(*t);
        return id ? std::optional(*id) : std::nullopt;
    }

    [[nodiscard]] std::span<const std::byte> rest() noexcept { return std::exchange(rest_, {}); }
    [[nodiscard]] bool empty() const noexcept { return rest_.empty(); }

private:
    std::span<const std::byte> rest_;
};

std::optional<Frame> fields_of(Type type, Cursor& in) noexcept {
    switch (type) {
    case Type::Hello: {
        const auto version = in.u8();
        const auto node = in.short_text().transform(core::NodeId::parse);
        if (!version || !node || !*node) {
            return std::nullopt;
        }
        return Hello{.version = *version, .node = **node};
    }
    case Type::Subscribe: {
        const auto request = in.u64();
        const auto room = in.room();
        if (!request || !room) {
            return std::nullopt;
        }
        return Subscribe{.request = *request, .room = *room};
    }
    case Type::Unsubscribe: {
        const auto room = in.room();
        if (!room) {
            return std::nullopt;
        }
        return Unsubscribe{.room = *room};
    }
    case Type::Send: {
        const auto request = in.u64();
        const auto room = in.room();
        const auto sender = in.sender();
        if (!request || !room || !sender) {
            return std::nullopt;
        }
        return Send{.request = *request, .room = *room, .sender = *sender, .body = in.rest()};
    }
    case Type::Reply: {
        const auto request = in.u64();
        const auto status = in.u8();
        const auto seq = in.u64();
        if (!request || !status || *status > static_cast<std::uint8_t>(Status::Busy) || !seq) {
            return std::nullopt;
        }
        return Reply{.request = *request, .status = static_cast<Status>(*status), .seq = *seq};
    }
    case Type::Deliver: {
        const auto room = in.room();
        const auto seq = in.u64();
        const auto sender = in.sender();
        if (!room || !seq || !sender) {
            return std::nullopt;
        }
        return Deliver{.room = *room, .seq = *seq, .sender = *sender, .body = in.rest()};
    }
    }
    return std::nullopt;
}

std::optional<Frame> parse(Type type, Cursor in) noexcept {
    std::optional<Frame> frame = fields_of(type, in);
    if (!in.empty()) {
        return std::nullopt;
    }
    return frame;
}

std::optional<Type> type_of(std::uint8_t byte) noexcept {
    if (byte < static_cast<std::uint8_t>(Type::Hello) ||
        byte > static_cast<std::uint8_t>(Type::Deliver)) {
        return std::nullopt;
    }
    return static_cast<Type>(byte);
}

} // namespace

void encode_hello(std::vector<std::byte>& out, const core::NodeId& node) {
    const std::size_t at = begin(out, Type::Hello);
    put_u8(out, kVersion);
    put_short(out, node.view());
    end(out, at);
}

void encode_subscribe(std::vector<std::byte>& out, std::uint64_t request,
                      const core::RoomId& room) {
    const std::size_t at = begin(out, Type::Subscribe);
    put_u64(out, request);
    put_room(out, room);
    end(out, at);
}

void encode_unsubscribe(std::vector<std::byte>& out, const core::RoomId& room) {
    const std::size_t at = begin(out, Type::Unsubscribe);
    put_room(out, room);
    end(out, at);
}

void encode_send(std::vector<std::byte>& out, std::uint64_t request, const core::RoomId& room,
                 const core::UserId& sender, std::span<const std::byte> body) {
    const std::size_t at = begin(out, Type::Send);
    put_u64(out, request);
    put_room(out, room);
    put_short(out, sender.view());
    out.insert(out.end(), body.begin(), body.end());
    end(out, at);
}

void encode_reply(std::vector<std::byte>& out, std::uint64_t request, Status status,
                  std::uint64_t seq) {
    const std::size_t at = begin(out, Type::Reply);
    put_u64(out, request);
    put_u8(out, static_cast<std::uint8_t>(status));
    put_u64(out, seq);
    end(out, at);
}

void encode_deliver(std::vector<std::byte>& out, const core::RoomId& room, std::uint64_t seq,
                    const core::UserId& sender, std::span<const std::byte> body) {
    const std::size_t at = begin(out, Type::Deliver);
    put_room(out, room);
    put_u64(out, seq);
    put_short(out, sender.view());
    out.insert(out.end(), body.begin(), body.end());
    end(out, at);
}

void Decoder::feed(std::span<const std::byte> bytes) {
    if (consumed_ > 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(consumed_));
        consumed_ = 0;
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

std::expected<std::optional<Frame>, DecodeError> Decoder::next() {
    if (failed_) {
        return std::unexpected(DecodeError::Malformed);
    }
    Cursor in{std::span{buffer_}.subspan(consumed_)};
    const auto header = in.take(kLengthBytes);
    if (!header) {
        return std::nullopt;
    }
    std::uint32_t length = 0;
    for (const std::byte b : *header) {
        length = (length << 8U) | std::to_integer<std::uint32_t>(b);
    }
    if (length == 0 || length > kMaxFrame) {
        failed_ = true;
        return std::unexpected(DecodeError::BadLength);
    }
    const auto payload = in.take(length);
    if (!payload) {
        return std::nullopt;
    }
    consumed_ += kLengthBytes + length;
    Cursor fields{*payload};
    const auto type = fields.u8().and_then(type_of);
    if (!type) {
        failed_ = true;
        return std::unexpected(DecodeError::UnknownType);
    }
    auto frame = parse(*type, fields);
    if (!frame) {
        failed_ = true;
        return std::unexpected(DecodeError::Malformed);
    }
    return frame;
}

} // namespace rt::wire
