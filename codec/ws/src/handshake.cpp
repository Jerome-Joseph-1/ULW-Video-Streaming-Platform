#include "codec/ws/handshake.hpp"

#include "http/method.hpp"
#include "http/request.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <openssl/evp.h>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>

namespace codec::ws {
namespace {

// RFC 6455 section 1.3.
constexpr std::string_view kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
// 16 random bytes in base64: 22 characters and "==".
constexpr std::size_t kKeyLength = 24;
constexpr std::size_t kSha1Bytes = 20;

char lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    return std::ranges::equal(a, b, [](char x, char y) { return lower(x) == lower(y); });
}

std::string_view trim(std::string_view s) noexcept {
    constexpr std::string_view kWhitespace = " \t";
    const std::size_t first = s.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(kWhitespace) - first + 1);
}

// Connection and Upgrade are comma-separated lists and may each be sent more than once.
bool has_token(std::span<const http::HeaderField> headers, std::string_view name,
               std::string_view token) noexcept {
    return std::ranges::any_of(headers, [&](const http::HeaderField& f) {
        if (!iequals(f.name, name)) {
            return false;
        }
        return std::ranges::any_of(std::views::split(f.value, ','), [&](auto part) {
            return iequals(trim(std::string_view{part.begin(), part.end()}), token);
        });
    });
}

// The fields RFC 6455 allows only once; a second copy would leave the answer ambiguous.
std::optional<std::string_view> single(std::span<const http::HeaderField> headers,
                                       std::string_view name) noexcept {
    std::optional<std::string_view> found;
    for (const http::HeaderField& f : headers) {
        if (!iequals(f.name, name)) {
            continue;
        }
        if (found) {
            return std::nullopt;
        }
        found = f.value;
    }
    return found;
}

bool is_base64(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' ||
           c == '/';
}

// The 22nd character carries the last 2 bits of the 16 bytes and 4 bits of padding, which
// canonical base64 leaves zero: only A, Q, g and w (0, 16, 32, 48) can end a real 16-byte key.
bool is_valid_key(std::string_view key) noexcept {
    constexpr std::string_view kLastCharacters = "AQgw";
    return key.size() == kKeyLength && key.ends_with("==") &&
           std::ranges::all_of(key.substr(0, kKeyLength - 2), is_base64) &&
           kLastCharacters.contains(key[kKeyLength - 3]);
}

} // namespace

std::optional<AcceptKey> accept_key(std::string_view key) noexcept {
    std::array<char, 64> input{};
    if (key.size() + kGuid.size() > input.size()) {
        return std::nullopt;
    }
    auto* const end = std::ranges::copy(key, input.begin()).out;
    std::ranges::copy(kGuid, end);
    std::array<unsigned char, kSha1Bytes> digest{};
    if (EVP_Digest(input.data(), key.size() + kGuid.size(), digest.data(), nullptr, EVP_sha1(),
                   nullptr) != 1) {
        return std::nullopt;
    }
    // EVP_EncodeBlock writes a terminating NUL after the 28 characters.
    std::array<unsigned char, std::tuple_size_v<AcceptKey> + 1> encoded{};
    EVP_EncodeBlock(encoded.data(), digest.data(), static_cast<int>(digest.size()));
    AcceptKey out{};
    std::ranges::transform(std::span{encoded}.first(out.size()), out.begin(),
                           [](unsigned char c) { return static_cast<char>(c); });
    return out;
}

UpgradeResponse::UpgradeResponse(const AcceptKey& key) noexcept {
    auto* out = std::ranges::copy(kHead, bytes_.begin()).out;
    out = std::ranges::copy(key, out).out;
    std::ranges::copy(kTail, out);
}

std::expected<UpgradeResponse, HandshakeError>
accept_handshake(const http::RequestHead& head) noexcept {
    if (head.method != http::Method::Get) {
        return std::unexpected(HandshakeError::NotGet);
    }
    if (head.version_minor < 1 || head.content_length != 0 ||
        !http::find_header(head.headers, "host")) {
        return std::unexpected(HandshakeError::Malformed);
    }
    if (!has_token(head.headers, "upgrade", "websocket") ||
        !has_token(head.headers, "connection", "upgrade")) {
        return std::unexpected(HandshakeError::NotAnUpgrade);
    }
    if (single(head.headers, "sec-websocket-version") != "13") {
        return std::unexpected(HandshakeError::UnsupportedVersion);
    }
    const std::optional<std::string_view> key = single(head.headers, "sec-websocket-key");
    if (!key || !is_valid_key(*key)) {
        return std::unexpected(HandshakeError::BadKey);
    }
    const std::optional<AcceptKey> accept = accept_key(*key);
    if (!accept) {
        return std::unexpected(HandshakeError::DigestFailed);
    }
    return UpgradeResponse{*accept};
}

std::string_view rejection_response(HandshakeError error) noexcept {
    switch (error) {
    case HandshakeError::NotGet:
        return "HTTP/1.1 405 Method Not Allowed\r\nAllow: GET\r\nContent-Length: 0\r\n"
               "Connection: close\r\n\r\n";
    case HandshakeError::Malformed:
    case HandshakeError::BadKey:
        return "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    // RFC 6455 section 4.2.2 and RFC 9110 section 15.5.22: name what the client must use.
    case HandshakeError::NotAnUpgrade:
    case HandshakeError::UnsupportedVersion:
        return "HTTP/1.1 426 Upgrade Required\r\nUpgrade: websocket\r\n"
               "Sec-WebSocket-Version: 13\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    case HandshakeError::DigestFailed:
        return "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n"
               "Connection: close\r\n\r\n";
    }
    return "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
}

} // namespace codec::ws
