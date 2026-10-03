#pragma once

#include <cstdint>
#include <expected>
#include <string_view>

namespace infra::auth {

enum class TokenSourceError : std::uint8_t {
    Missing,
    // An Authorization header other than "Bearer <token>", or a token holding characters no
    // compact JWS has.
    Malformed,
    // Two Authorization headers, or the token cookie twice. A sibling subdomain can plant a
    // second cookie of the same name, and nothing says which of the two the user holds.
    Ambiguous,
};

// Finds the token in a request: `Authorization: Bearer <token>`, else the cookie named
// `cookie_name` (ULW_AUTH_COOKIE, auth_token by default). Fed every header of the request;
// all others, x-user-* included, are ignored, never trusted. The token views the header value
// it came from.
class TokenExtractor {
public:
    explicit TokenExtractor(std::string_view cookie_name) noexcept : cookie_name_(cookie_name) {}

    void on_header(std::string_view name, std::string_view value) noexcept;

    // An Authorization header wins over the cookie: the browser attaches the cookie to every
    // request, while the header is what the client chose to send. A malformed header is
    // refused rather than passed over for the cookie.
    [[nodiscard]] std::expected<std::string_view, TokenSourceError> token() const noexcept;

private:
    void on_cookies(std::string_view header) noexcept;

    std::string_view cookie_name_;
    std::string_view authorization_;
    std::string_view cookie_;
    unsigned authorization_count_ = 0;
    unsigned cookie_count_ = 0;
};

} // namespace infra::auth
