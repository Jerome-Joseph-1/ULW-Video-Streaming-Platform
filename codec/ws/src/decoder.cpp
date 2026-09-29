#include "codec/ws/decoder.hpp"

#include "codec/ws/frame.hpp"
#include "codec/ws/utf8.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <utility>

namespace codec::ws {
namespace {

// RFC 6455 section 5.2.
constexpr std::uint8_t kFin = 0x80;
constexpr std::uint8_t kRsv = 0x70;
constexpr std::uint8_t kOpcodeBits = 0x0F;
constexpr std::uint8_t kMasked = 0x80;
constexpr std::uint8_t kLength7 = 0x7F;
constexpr std::uint8_t kLength16 = 126;
constexpr std::uint8_t kLength64 = 127;
constexpr std::size_t kMaskBytes = 4;

std::uint8_t u8(std::byte b) noexcept {
    return std::to_integer<std::uint8_t>(b);
}

std::optional<Opcode> known_opcode(std::uint8_t bits) noexcept {
    switch (bits) {
    case 0x0:
        return Opcode::Continuation;
    case 0x1:
        return Opcode::Text;
    case 0x2:
        return Opcode::Binary;
    case 0x8:
        return Opcode::Close;
    case 0x9:
        return Opcode::Ping;
    case 0xA:
        return Opcode::Pong;
    default:
        return std::nullopt;
    }
}

std::size_t extended_length_bytes(std::uint8_t length7) noexcept {
    switch (length7) {
    case kLength16:
        return 2;
    case kLength64:
        return 8;
    default:
        return 0;
    }
}

std::uint64_t big_endian(std::span<const std::byte> bytes) noexcept {
    std::uint64_t v = 0;
    for (const std::byte b : bytes) {
        v = (v << 8U) | u8(b);
    }
    return v;
}

} // namespace

Decoder::Decoder(std::uint64_t max_message_bytes) noexcept : max_message_(max_message_bytes) {}

Decoded Decoder::feed(std::span<const std::byte> bytes) {
    Decoded out;
    while (true) {
        switch (state_) {
        case State::Failed:
            out.error = error_;
            return out;
        case State::Closed:
            return out;
        case State::Header:
            if (bytes.empty()) {
                return out;
            }
            bytes = take_header(bytes, out);
            break;
        case State::Payload:
            if (bytes.empty()) {
                return out;
            }
            bytes = take_payload(bytes, out);
            break;
        }
    }
}

std::span<const std::byte> Decoder::take_header(std::span<const std::byte> bytes, Decoded& out) {
    const std::size_t n = std::min(header_need_ - header_have_, bytes.size());
    std::ranges::copy(bytes.first(n), std::span{header_}.subspan(header_have_).begin());
    header_have_ += n;
    bytes = bytes.subspan(n);
    if (header_have_ < header_need_) {
        return bytes;
    }
    if (header_have_ == 2) {
        if (const auto code = read_first_two()) {
            fail(*code);
            return bytes;
        }
        header_need_ = 2 + extended_length_bytes(u8(header_[1]) & kLength7) + kMaskBytes;
        return bytes;
    }
    if (const auto code = read_rest_of_header()) {
        fail(*code);
        return bytes;
    }
    header_have_ = 0;
    header_need_ = 2;
    if (remaining_ == 0) {
        finish_frame(out);
    } else {
        state_ = State::Payload;
    }
    return bytes;
}

// Everything that makes a frame unacceptable, except its length, shows in its first two bytes,
// so it is refused before the peer can make us wait for (or store) any more of it.
std::optional<CloseCode> Decoder::read_first_two() noexcept {
    const std::uint8_t b0 = u8(header_[0]);
    const std::uint8_t b1 = u8(header_[1]);
    // RSV bits need an extension that defines them, and none is ever negotiated.
    if ((b0 & kRsv) != 0) {
        return CloseCode::ProtocolError;
    }
    const std::optional<Opcode> op = known_opcode(b0 & kOpcodeBits);
    if (!op) {
        return CloseCode::ProtocolError;
    }
    // RFC 6455 section 5.1: a server closes the connection on an unmasked client frame.
    if ((b1 & kMasked) == 0) {
        return CloseCode::ProtocolError;
    }
    opcode_ = *op;
    fin_ = (b0 & kFin) != 0;
    if (is_control(opcode_)) {
        // A length field of 126 or 127 announces an extended length, so this also refuses
        // every control frame longer than 125 bytes before its length is read.
        if (!fin_ || (b1 & kLength7) > kMaxControlPayload) {
            return CloseCode::ProtocolError;
        }
        return std::nullopt;
    }
    const bool continues = opcode_ == Opcode::Continuation;
    if (continues != message_opcode_.has_value()) {
        return CloseCode::ProtocolError;
    }
    return std::nullopt;
}

std::optional<CloseCode> Decoder::read_rest_of_header() noexcept {
    const std::span<const std::byte> header = std::span{header_}.first(header_have_);
    const std::uint8_t length7 = u8(header[1]) & kLength7;
    const std::span<const std::byte> extended = header.subspan(2, header.size() - 2 - kMaskBytes);
    const std::uint64_t length = extended.empty() ? length7 : big_endian(extended);
    // RFC 6455 section 5.2: the most significant bit of a 64-bit length must be 0.
    if (length7 == kLength64 && (length >> 63U) != 0) {
        return CloseCode::ProtocolError;
    }
    if (!is_control(opcode_) && length > max_message_ - message_.size()) {
        return CloseCode::MessageTooBig;
    }
    // Loaded little-endian so that the low byte is the key's first byte, whichever way the
    // payload is split afterwards.
    mask_ = 0;
    for (const std::byte b : header.last(kMaskBytes) | std::views::reverse) {
        mask_ = (mask_ << 8U) | u8(b);
    }
    remaining_ = length;
    control_size_ = 0;
    if (opcode_ == Opcode::Text || opcode_ == Opcode::Binary) {
        message_opcode_ = opcode_;
        utf8_.reset();
    }
    return std::nullopt;
}

std::span<const std::byte> Decoder::take_payload(std::span<const std::byte> bytes, Decoded& out) {
    const std::size_t n = std::min<std::uint64_t>(remaining_, bytes.size());
    const std::span<const std::byte> masked = bytes.first(n);
    if (is_control(opcode_)) {
        unmask_into(masked, std::span{control_}.subspan(control_size_, n));
        control_size_ += n;
    } else {
        const std::size_t old = message_.size();
        grow_message(old + n);
        message_.resize(old + n);
        const std::span<std::byte> plain = std::span{message_}.subspan(old);
        unmask_into(masked, plain);
        // Checked as the bytes arrive, so a message is refused at its first invalid byte, not
        // after the peer has made us store the rest of it.
        if (message_opcode_ == Opcode::Text && !utf8_.feed(plain)) {
            fail(CloseCode::InvalidPayload);
            return {};
        }
    }
    remaining_ -= n;
    if (remaining_ == 0) {
        state_ = State::Header;
        finish_frame(out);
    }
    return bytes.subspan(n);
}

// Doubling keeps a message sent one byte per frame from costing a reallocation per byte, and
// the cap keeps the doubling from reserving past the limit.
void Decoder::grow_message(std::size_t size) {
    if (size <= message_.capacity()) {
        return;
    }
    const std::uint64_t doubled = std::uint64_t{2} * message_.capacity();
    message_.reserve(std::max<std::uint64_t>(size, std::min(doubled, max_message_)));
}

void Decoder::unmask_into(std::span<const std::byte> masked, std::span<std::byte> out) noexcept {
    std::ranges::transform(masked, out.begin(), [this](std::byte b) noexcept {
        const auto key = static_cast<std::byte>(mask_ & 0xFFU);
        mask_ = std::rotr(mask_, 8);
        return b ^ key;
    });
}

void Decoder::finish_frame(Decoded& out) {
    if (opcode_ == Opcode::Close) {
        finish_close(out);
        return;
    }
    if (is_control(opcode_)) {
        const std::span<const std::byte> payload = std::span{control_}.first(control_size_);
        out.frames.push_back({.opcode = opcode_,
                              .fin = true,
                              .payload = {payload.begin(), payload.end()},
                              .close_code = CloseCode::NoStatus});
        return;
    }
    if (!fin_) {
        return;
    }
    const Opcode op = *message_opcode_;
    if (op == Opcode::Text && !utf8_.at_boundary()) {
        fail(CloseCode::InvalidPayload);
        return;
    }
    out.frames.push_back({.opcode = op,
                          .fin = true,
                          .payload = std::exchange(message_, {}),
                          .close_code = CloseCode::NoStatus});
    message_opcode_.reset();
}

void Decoder::finish_close(Decoded& out) {
    const std::span<const std::byte> payload = std::span{control_}.first(control_size_);
    Frame close{
        .opcode = Opcode::Close, .fin = true, .payload = {}, .close_code = CloseCode::NoStatus};
    if (payload.size() == 1) {
        fail(CloseCode::ProtocolError);
        return;
    }
    if (!payload.empty()) {
        const CloseCode code{static_cast<std::uint16_t>(big_endian(payload.first(2)))};
        if (!is_valid_on_wire(code)) {
            fail(CloseCode::ProtocolError);
            return;
        }
        const std::span<const std::byte> reason = payload.subspan(2);
        if (!is_valid_utf8(reason)) {
            fail(CloseCode::InvalidPayload);
            return;
        }
        close.close_code = code;
        close.payload.assign(reason.begin(), reason.end());
    }
    out.frames.push_back(std::move(close));
    state_ = State::Closed;
}

void Decoder::fail(CloseCode code) noexcept {
    error_ = code;
    state_ = State::Failed;
}

} // namespace codec::ws
