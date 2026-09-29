#pragma once

#include "http/request.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

namespace codec::ws {

enum class HandshakeError : std::uint8_t {
    NotGet,
    // Not HTTP/1.1, no Host, or a body.
    Malformed,
    // Sec-WebSocket-Key missing, repeated, or not 16 bytes in base64.
    BadKey,
    // No "Upgrade: websocket" or no "Connection: upgrade".
    NotAnUpgrade,
    // Sec-WebSocket-Version missing, repeated, or not 13.
    UnsupportedVersion,
    // OpenSSL could not compute SHA-1.
    DigestFailed,
};

// Sec-WebSocket-Accept: base64 of the SHA-1 of the key and the RFC 6455 GUID, 28 characters.
using AcceptKey = std::array<char, 28>;

[[nodiscard]] std::optional<AcceptKey> accept_key(std::string_view key) noexcept;

// The complete 101 response. No subprotocol and no extension is ever agreed, so a client that
// offers permessage-deflate is answered without it and sends uncompressed frames.
class UpgradeResponse {
public:
    explicit UpgradeResponse(const AcceptKey& key) noexcept;

    [[nodiscard]] std::string_view bytes() const noexcept { return {bytes_.data(), bytes_.size()}; }

private:
    static constexpr std::string_view kHead = "HTTP/1.1 101 Switching Protocols\r\n"
                                              "Upgrade: websocket\r\n"
                                              "Connection: Upgrade\r\n"
                                              "Sec-WebSocket-Accept: ";
    static constexpr std::string_view kTail = "\r\n\r\n";

    std::array<char, kHead.size() + std::tuple_size_v<AcceptKey> + kTail.size()> bytes_{};
};

// RFC 6455 section 4.2.1, on a head from http::RequestParser.
[[nodiscard]] std::expected<UpgradeResponse, HandshakeError>
accept_handshake(const http::RequestHead& head) noexcept;

// A complete bodiless response that refuses the upgrade and closes the connection.
[[nodiscard]] std::string_view rejection_response(HandshakeError error) noexcept;

} // namespace codec::ws
