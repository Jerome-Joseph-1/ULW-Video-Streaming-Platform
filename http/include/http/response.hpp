#pragma once

#include "http/method.hpp"
#include "http/status.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace http {

// Always sent explicitly: an HTTP/1.0 client that asked for keep-alive closes without it.
enum class Connection : std::uint8_t { KeepAlive, Close };

// A complete bodiless response: status line, Content-Length: 0 (none on a 204) and
// Connection, built at compile time. A response that needs any other field (a 405's Allow,
// a 503's Retry-After) goes through write_response_head().
[[nodiscard]] std::string_view fixed_response(Status status, Connection connection) noexcept;

// Empty strings and disengaged optionals are left out. A 204 never carries Content-Length
// (RFC 9110 section 8.6), so content_length is ignored for it.
//
// Every member has an initializer so a designated initializer may name only the fields it
// sets; GCC's -Wmissing-field-initializers flags the others otherwise.
// NOLINTBEGIN(readability-redundant-member-init)
struct ResponseHead {
    Status status = Status::Ok;
    std::uint64_t content_length = 0;
    Connection connection = Connection::KeepAlive;
    std::string_view content_type = {};
    std::string_view cache_control = {};
    std::string_view location = {};
    std::optional<std::uint64_t> upload_offset = std::nullopt;
    std::optional<std::chrono::seconds> retry_after = std::nullopt;
    // A 401's challenge (RFC 9110 section 11.6.1): `Bearer`, with an error for a token that
    // was sent and refused (RFC 6750 section 3).
    std::string_view www_authenticate = {};
    MethodSet allow = {};
    std::string_view request_id = {};
};
// NOLINTEND(readability-redundant-member-init)

enum class WriteError : std::uint8_t {
    BufferTooSmall,
    // A control character in a value would let it end the field, or the head, early.
    InvalidFieldValue,
};

// Writes everything up to and including the blank line; the caller sends the body after it.
[[nodiscard]] std::expected<std::size_t, WriteError>
write_response_head(const ResponseHead& head, std::span<char> out) noexcept;

} // namespace http
