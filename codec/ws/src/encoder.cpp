#include "codec/ws/encoder.hpp"

#include "codec/ws/frame.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

namespace codec::ws {
namespace {

// RFC 6455 section 5.2.
constexpr std::uint8_t kFin = 0x80;
constexpr std::uint8_t kMasked = 0x80;
constexpr std::uint8_t kLength16 = 126;
constexpr std::uint8_t kLength64 = 127;
constexpr std::size_t kMaxControlPayload = 125;
constexpr std::size_t kCloseStatusBytes = 2;

using MaskKey = std::array<std::byte, 4>;

// The largest header: two bytes, a 64-bit length and a mask key.
constexpr std::size_t kMaxHeaderBytes = 2 + 8 + 4;

// What encode_frame reads of a frame: a Frame's fields, or a payload the caller holds elsewhere.
struct FrameView {
    Opcode opcode;
    bool fin;
    std::span<const std::byte> payload;
    CloseCode close_code;
};

std::expected<std::size_t, EncodeError> payload_size(const FrameView& frame) noexcept {
    std::size_t size = frame.payload.size();
    if (frame.opcode == Opcode::Close && frame.close_code != CloseCode::NoStatus) {
        if (!is_valid_on_wire(frame.close_code)) {
            return std::unexpected(EncodeError::InvalidClose);
        }
        size += kCloseStatusBytes;
    } else if (frame.opcode == Opcode::Close && !frame.payload.empty()) {
        return std::unexpected(EncodeError::InvalidClose);
    }
    if (is_control(frame.opcode)) {
        if (!frame.fin) {
            return std::unexpected(EncodeError::ControlFragmented);
        }
        if (size > kMaxControlPayload) {
            return std::unexpected(EncodeError::ControlTooLong);
        }
    }
    return size;
}

void append_big_endian(std::uint64_t value, std::size_t bytes, std::vector<std::byte>& out) {
    for (std::size_t i = bytes; i > 0; --i) {
        out.push_back(static_cast<std::byte>((value >> (8 * (i - 1))) & 0xFFU));
    }
}

void append_header(const FrameView& frame, std::uint64_t size, const std::optional<MaskKey>& mask,
                   std::vector<std::byte>& out) {
    const auto first = static_cast<std::uint8_t>((frame.fin ? kFin : 0U) |
                                                 static_cast<std::uint8_t>(frame.opcode));
    const std::uint8_t masked = mask ? kMasked : 0U;
    out.push_back(std::byte{first});
    if (size < kLength16) {
        out.push_back(static_cast<std::byte>(masked | size));
    } else if (size <= std::numeric_limits<std::uint16_t>::max()) {
        out.push_back(static_cast<std::byte>(masked | kLength16));
        append_big_endian(size, 2, out);
    } else {
        out.push_back(static_cast<std::byte>(masked | kLength64));
        append_big_endian(size, 8, out);
    }
    if (mask) {
        out.insert(out.end(), mask->begin(), mask->end());
    }
}

void append_payload(const FrameView& frame, std::vector<std::byte>& out) {
    if (frame.opcode == Opcode::Close && frame.close_code != CloseCode::NoStatus) {
        append_big_endian(frame.close_code.value, kCloseStatusBytes, out);
    }
    out.insert(out.end(), frame.payload.begin(), frame.payload.end());
}

std::expected<void, EncodeError> encode_frame(const FrameView& frame,
                                              const std::optional<MaskKey>& mask,
                                              std::vector<std::byte>& out) {
    const auto size = payload_size(frame);
    if (!size) {
        return std::unexpected(size.error());
    }
    // At most one growth for the frame, instead of one per header byte and one for the payload;
    // still geometric, for a caller that appends frame after frame to one buffer.
    if (const std::size_t need = out.size() + kMaxHeaderBytes + *size; need > out.capacity()) {
        out.reserve(std::max(need, 2 * out.capacity()));
    }
    append_header(frame, *size, mask, out);
    const std::size_t start = out.size();
    append_payload(frame, out);
    if (mask) {
        std::uint32_t key = 0;
        for (const std::byte b : *mask | std::views::reverse) {
            key = (key << 8U) | std::to_integer<std::uint32_t>(b);
        }
        for (std::byte& b : std::span{out}.subspan(start)) {
            b ^= static_cast<std::byte>(key & 0xFFU);
            key = std::rotr(key, 8);
        }
    }
    return {};
}

FrameView view_of(const Frame& frame) noexcept {
    return {.opcode = frame.opcode,
            .fin = frame.fin,
            .payload = frame.payload,
            .close_code = frame.close_code};
}

} // namespace

std::expected<void, EncodeError> encode(const Frame& frame, std::vector<std::byte>& out) {
    return encode_frame(view_of(frame), std::nullopt, out);
}

std::expected<void, EncodeError> encode(Opcode opcode, bool fin, std::span<const std::byte> payload,
                                        std::vector<std::byte>& out) {
    return encode_frame(
        {.opcode = opcode, .fin = fin, .payload = payload, .close_code = CloseCode::NoStatus},
        std::nullopt, out);
}

std::expected<void, EncodeError> ClientEncoder::encode(const Frame& frame,
                                                       std::vector<std::byte>& out) {
    MaskKey key{};
    keys_.fill(key);
    return encode_frame(view_of(frame), key, out);
}

} // namespace codec::ws
