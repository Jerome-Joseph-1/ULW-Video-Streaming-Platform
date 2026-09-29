#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace codec::ws {

// RFC 6455 section 5.2. The reserved opcodes never get past the decoder.
enum class Opcode : std::uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

[[nodiscard]] constexpr bool is_control(Opcode op) noexcept {
    return (static_cast<std::uint8_t>(op) & 0x8U) != 0;
}

// A Close status (RFC 6455 section 7.4). Not an enum: a peer may send any code
// is_valid_on_wire() accepts, 3000-4999 included, and the codec passes it on as it came.
struct CloseCode {
    std::uint16_t value;

    static const CloseCode Normal;
    static const CloseCode GoingAway;
    static const CloseCode ProtocolError;
    // Never on the wire: stands for a Close that carried no status at all.
    static const CloseCode NoStatus;
    static const CloseCode InvalidPayload;
    static const CloseCode MessageTooBig;

    friend constexpr bool operator==(CloseCode, CloseCode) noexcept = default;
};

inline constexpr CloseCode CloseCode::Normal{1000};
inline constexpr CloseCode CloseCode::GoingAway{1001};
inline constexpr CloseCode CloseCode::ProtocolError{1002};
inline constexpr CloseCode CloseCode::NoStatus{1005};
inline constexpr CloseCode CloseCode::InvalidPayload{1007};
inline constexpr CloseCode CloseCode::MessageTooBig{1009};

// RFC 6455 section 7.4 and the IANA "WebSocket Close Code Number" registry. 1004 is reserved,
// 1005, 1006 and 1015 are for reporting locally and must not be sent, everything else below
// 3000 is unassigned or reserved for the protocol, and nothing is defined above 4999.
[[nodiscard]] constexpr bool is_valid_on_wire(CloseCode code) noexcept {
    const std::uint16_t c = code.value;
    return (c >= 1000 && c <= 1003) || (c >= 1007 && c <= 1014) || (c >= 3000 && c <= 4999);
}

// A whole message (Text, Binary) or a control frame. The decoder delivers only final frames,
// because it reassembles fragments; `fin` and Continuation are for encoding a message in parts.
struct Frame {
    Opcode opcode = Opcode::Binary;
    bool fin = true;
    // Unmasked. A Close keeps its reason here and its status in close_code.
    std::vector<std::byte> payload;
    CloseCode close_code = CloseCode::NoStatus;

    friend bool operator==(const Frame&, const Frame&) = default;
};

} // namespace codec::ws
