#pragma once

#include "codec/ws/frame.hpp"
#include "core/ports/random.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace codec::ws {

enum class EncodeError : std::uint8_t {
    // A control frame carries at most 125 bytes, a Close's two status bytes included.
    ControlTooLong,
    ControlFragmented,
    // A status is_valid_on_wire() refuses, or a reason on a Close without a status.
    InvalidClose,
};

// Appends one server frame, unmasked, with the shortest length encoding for its payload. The
// caller keeps the sequence valid: Continuation only inside a message it started with fin unset.
[[nodiscard]] std::expected<void, EncodeError> encode(const Frame& frame,
                                                      std::vector<std::byte>& out);
// The same for a payload the caller already holds, which is not copied into a Frame first. With
// no close status, a Close takes no payload.
[[nodiscard]] std::expected<void, EncodeError>
encode(Opcode opcode, bool fin, std::span<const std::byte> payload, std::vector<std::byte>& out);

// The client's side, for tests and tools: every frame masked with a fresh key.
class ClientEncoder {
public:
    // RFC 6455 section 5.3 asks for keys the server cannot predict; `keys` decides whether
    // they are.
    explicit ClientEncoder(core::ports::IRandom& keys) noexcept : keys_(keys) {}

    [[nodiscard]] std::expected<void, EncodeError> encode(const Frame& frame,
                                                          std::vector<std::byte>& out);

private:
    core::ports::IRandom& keys_;
};

} // namespace codec::ws
