#include "http/response.hpp"

#include "http/method.hpp"
#include "http/status.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string_view>
#include <utility>

namespace http {

namespace {

// The longest, a keep-alive 431, is 91 bytes.
constexpr std::size_t kFixedCapacity = 128;

class FixedBytes {
public:
    // at() makes an overflow a compile error in the consteval builder below.
    constexpr void append(std::string_view text) {
        for (const char c : text) {
            bytes_.at(size_++) = c;
        }
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return {bytes_.data(), size_};
    }

private:
    std::array<char, kFixedCapacity> bytes_{};
    std::size_t size_ = 0;
};

consteval FixedBytes make_fixed(Status status, Connection connection) {
    const std::uint16_t n = code(status);
    const std::array digits{static_cast<char>('0' + (n / 100)),
                            static_cast<char>('0' + (n / 10 % 10)),
                            static_cast<char>('0' + (n % 10))};
    FixedBytes out;
    out.append("HTTP/1.1 ");
    out.append({digits.data(), digits.size()});
    out.append(" ");
    out.append(reason_phrase(status));
    out.append("\r\n");
    if (status != Status::NoContent) {
        out.append("Content-Length: 0\r\n");
    }
    out.append(connection == Connection::Close ? "Connection: close\r\n\r\n"
                                               : "Connection: keep-alive\r\n\r\n");
    return out;
}

template <Status S> std::string_view fixed(Connection connection) noexcept {
    static constexpr FixedBytes kKeepAlive = make_fixed(S, Connection::KeepAlive);
    static constexpr FixedBytes kClose = make_fixed(S, Connection::Close);
    return connection == Connection::Close ? kClose.view() : kKeepAlive.view();
}

constexpr std::array<std::pair<Method, std::string_view>, 7> kAllowTokens{{
    {Method::Get, "GET"},
    {Method::Head, "HEAD"},
    {Method::Post, "POST"},
    {Method::Put, "PUT"},
    {Method::Patch, "PATCH"},
    {Method::Delete, "DELETE"},
    {Method::Options, "OPTIONS"},
}};

// RFC 9110 field-value: no C0 control but HTAB, no DEL; obs-text (0x80 and up) is allowed.
bool is_field_value(std::string_view value) noexcept {
    return std::ranges::none_of(value, [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return (byte < 0x20 && c != '\t') || byte == 0x7F;
    });
}

class HeadWriter {
public:
    explicit HeadWriter(std::span<char> out) noexcept : out_(out) {}

    // format_to_n() throws only for a format string that std::format_string has already
    // rejected at compile time, and never allocates when writing through a pointer.
    template <typename... Args>
    // NOLINTNEXTLINE(bugprone-exception-escape)
    void put(std::format_string<Args...> format, Args&&... args) noexcept {
        if (overflow_) {
            return;
        }
        const std::span<char> rest = out_.subspan(used_);
        const auto written = std::format_to_n(rest.data(), static_cast<std::ptrdiff_t>(rest.size()),
                                              format, std::forward<Args>(args)...);
        const auto size = static_cast<std::size_t>(written.size);
        if (size > rest.size()) {
            overflow_ = true;
            return;
        }
        used_ += size;
    }

    void field(std::string_view name, std::string_view value) noexcept {
        if (!value.empty()) {
            put("{}: {}\r\n", name, value);
        }
    }

    [[nodiscard]] std::expected<std::size_t, WriteError> finish() const noexcept {
        if (overflow_) {
            return std::unexpected(WriteError::BufferTooSmall);
        }
        return used_;
    }

private:
    std::span<char> out_;
    std::size_t used_ = 0;
    bool overflow_ = false;
};

} // namespace

std::string_view fixed_response(Status status, Connection connection) noexcept {
    switch (status) {
    case Status::Ok:
        return fixed<Status::Ok>(connection);
    case Status::Created:
        return fixed<Status::Created>(connection);
    case Status::NoContent:
        return fixed<Status::NoContent>(connection);
    case Status::BadRequest:
        return fixed<Status::BadRequest>(connection);
    case Status::Unauthorized:
        return fixed<Status::Unauthorized>(connection);
    case Status::NotFound:
        return fixed<Status::NotFound>(connection);
    case Status::MethodNotAllowed:
        return fixed<Status::MethodNotAllowed>(connection);
    case Status::RequestTimeout:
        return fixed<Status::RequestTimeout>(connection);
    case Status::Conflict:
        return fixed<Status::Conflict>(connection);
    case Status::LengthRequired:
        return fixed<Status::LengthRequired>(connection);
    case Status::ContentTooLarge:
        return fixed<Status::ContentTooLarge>(connection);
    case Status::TooManyRequests:
        return fixed<Status::TooManyRequests>(connection);
    case Status::RequestHeaderFieldsTooLarge:
        return fixed<Status::RequestHeaderFieldsTooLarge>(connection);
    case Status::InternalServerError:
        return fixed<Status::InternalServerError>(connection);
    case Status::NotImplemented:
        return fixed<Status::NotImplemented>(connection);
    case Status::ServiceUnavailable:
        return fixed<Status::ServiceUnavailable>(connection);
    case Status::HttpVersionNotSupported:
        return fixed<Status::HttpVersionNotSupported>(connection);
    }
    // Only reachable by casting an unlisted code to Status.
    return fixed<Status::InternalServerError>(Connection::Close);
}

std::expected<std::size_t, WriteError> write_response_head(const ResponseHead& head,
                                                           std::span<char> out) noexcept {
    const std::array values{head.content_type, head.cache_control, head.location, head.request_id};
    if (!std::ranges::all_of(values, is_field_value) ||
        (head.retry_after && head.retry_after->count() < 0)) {
        return std::unexpected(WriteError::InvalidFieldValue);
    }

    HeadWriter w{out};
    w.put("HTTP/1.1 {} {}\r\n", code(head.status), reason_phrase(head.status));
    if (head.status != Status::NoContent) {
        w.put("Content-Length: {}\r\n", head.content_length);
    }
    w.put("Connection: {}\r\n", head.connection == Connection::Close ? "close" : "keep-alive");
    w.field("Content-Type", head.content_type);
    w.field("Cache-Control", head.cache_control);
    w.field("Location", head.location);
    if (head.upload_offset) {
        w.put("Upload-Offset: {}\r\n", *head.upload_offset);
    }
    if (head.retry_after) {
        w.put("Retry-After: {}\r\n", head.retry_after->count());
    }
    if (!head.allow.empty()) {
        std::string_view separator = "Allow: ";
        for (const auto& [method, token] : kAllowTokens) {
            if (head.allow.contains(method)) {
                w.put("{}{}", separator, token);
                separator = ", ";
            }
        }
        w.put("\r\n");
    }
    w.field("X-Request-Id", head.request_id);
    w.put("\r\n");
    return w.finish();
}

} // namespace http
