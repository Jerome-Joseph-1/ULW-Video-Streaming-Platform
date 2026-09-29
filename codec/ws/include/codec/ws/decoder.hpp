#pragma once

#include "codec/ws/frame.hpp"
#include "codec/ws/utf8.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace codec::ws {

struct Decoded {
    // Every message and control frame the bytes completed, in the order they arrived, including
    // those that came before an error.
    std::vector<Frame> frames;
    // Set once the peer has broken the protocol: the status of the Close to send before
    // closing the connection. Nothing more is decoded afterwards.
    std::optional<CloseCode> error;
};

// The server side of RFC 6455 without extensions: client frames arrive masked and are delivered
// unmasked, fragmented messages are reassembled, and control frames sent between the fragments
// of a message are delivered as they complete, ahead of the message.
//
// Memory is bounded by the protocol, not by the peer: the decoder holds at most one frame header
// (14 bytes), one control payload (125 bytes) and the message being reassembled, whose capacity
// never exceeds max_message_bytes. A frame is judged by its header, so a control frame too long
// or a message too big is refused before any of its payload is read. A finished message's
// payload is moved into its Frame, not copied: each byte is copied once, while it is unmasked.
//
// After a Close the peer sends nothing more (RFC 6455 section 5.5.1), so anything that follows
// one is ignored.
class Decoder {
public:
    // A chat message is at most 4,096 characters, 16 KiB of UTF-8, and its sealed body adds a
    // few hundred bytes; a signalling message carries an SDP offer of 5-10 KB. 64 KiB is four
    // times the largest of them, and bounds what a connection that stalls mid-message can hold.
    static constexpr std::uint64_t kDefaultMaxMessageBytes = std::uint64_t{64} * 1024;

    explicit Decoder(std::uint64_t max_message_bytes = kDefaultMaxMessageBytes) noexcept;

    [[nodiscard]] Decoded feed(std::span<const std::byte> bytes);

    [[nodiscard]] std::optional<CloseCode> error() const noexcept { return error_; }
    // A Close has been delivered.
    [[nodiscard]] bool closed() const noexcept { return state_ == State::Closed; }

private:
    enum class State : std::uint8_t { Header, Payload, Closed, Failed };

    // RFC 6455 section 5.2: 2 fixed bytes, up to 8 of extended length, 4 of masking key.
    static constexpr std::size_t kMaxHeaderBytes = 14;
    static constexpr std::size_t kMaxControlPayload = 125;

    [[nodiscard]] std::span<const std::byte> take_header(std::span<const std::byte> bytes,
                                                         Decoded& out);
    [[nodiscard]] std::span<const std::byte> take_payload(std::span<const std::byte> bytes,
                                                          Decoded& out);
    [[nodiscard]] std::optional<CloseCode> read_first_two() noexcept;
    [[nodiscard]] std::optional<CloseCode> read_rest_of_header() noexcept;
    void finish_frame(Decoded& out);
    void finish_close(Decoded& out);
    void fail(CloseCode code) noexcept;
    void unmask_into(std::span<const std::byte> masked, std::span<std::byte> out) noexcept;
    void grow_message(std::size_t size);

    std::uint64_t max_message_;
    State state_ = State::Header;
    std::optional<CloseCode> error_;

    std::array<std::byte, kMaxHeaderBytes> header_{};
    std::size_t header_have_ = 0;
    std::size_t header_need_ = 2;

    Opcode opcode_ = Opcode::Binary;
    bool fin_ = false;
    std::uint64_t remaining_ = 0;
    // The masking key rotated so that its low byte applies to the next payload byte.
    std::uint32_t mask_ = 0;

    std::array<std::byte, kMaxControlPayload> control_{};
    std::size_t control_size_ = 0;

    // The data opcode of the message being reassembled, if one is.
    std::optional<Opcode> message_opcode_;
    std::vector<std::byte> message_;
    Utf8Validator utf8_;
};

} // namespace codec::ws
