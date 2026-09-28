#include "infra/auth/token_extractor.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <string_view>

namespace infra::auth {

namespace {

bool equals_ignoring_case(std::string_view a, std::string_view b) noexcept {
    const auto lower = [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    };
    return a.size() == b.size() &&
           std::ranges::equal(a, b, [&](char x, char y) { return lower(x) == lower(y); });
}

bool is_ows(char c) noexcept {
    return c == ' ' || c == '\t';
}

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && is_ows(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && is_ows(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

// The base64url alphabet and the two dots of a compact JWS; the verifier checks the rest.
bool is_token_char(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_' || c == '.';
}

std::expected<std::string_view, TokenSourceError> checked(std::string_view token) noexcept {
    if (token.empty() || !std::ranges::all_of(token, is_token_char)) {
        return std::unexpected(TokenSourceError::Malformed);
    }
    return token;
}

// RFC 6750 section 2.1: "Bearer" 1*SP token, the scheme matched without regard to case.
std::expected<std::string_view, TokenSourceError> bearer(std::string_view value) noexcept {
    constexpr std::string_view kScheme = "Bearer";
    value = trim(value);
    if (value.size() <= kScheme.size() ||
        !equals_ignoring_case(value.substr(0, kScheme.size()), kScheme) ||
        value[kScheme.size()] != ' ') {
        return std::unexpected(TokenSourceError::Malformed);
    }
    value.remove_prefix(kScheme.size());
    while (!value.empty() && value.front() == ' ') {
        value.remove_prefix(1);
    }
    return checked(value);
}

} // namespace

void TokenExtractor::on_header(std::string_view name, std::string_view value) noexcept {
    if (equals_ignoring_case(name, "authorization")) {
        ++authorization_count_;
        authorization_ = value;
    } else if (equals_ignoring_case(name, "cookie")) {
        on_cookies(value);
    }
}

// RFC 6265 section 4.2.1: name=value pairs separated by "; ". Names compare exactly, and a
// value may arrive wrapped in double quotes.
void TokenExtractor::on_cookies(std::string_view header) noexcept {
    while (!header.empty()) {
        const std::size_t end = header.find(';');
        const std::string_view pair = trim(header.substr(0, end));
        header = end == std::string_view::npos ? std::string_view{} : header.substr(end + 1);
        const std::size_t eq = pair.find('=');
        if (eq == std::string_view::npos || trim(pair.substr(0, eq)) != cookie_name_) {
            continue;
        }
        std::string_view value = trim(pair.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        ++cookie_count_;
        cookie_ = value;
    }
}

std::expected<std::string_view, TokenSourceError> TokenExtractor::token() const noexcept {
    if (authorization_count_ > 1 || cookie_count_ > 1) {
        return std::unexpected(TokenSourceError::Ambiguous);
    }
    if (authorization_count_ == 1) {
        return bearer(authorization_);
    }
    // An emptied cookie is what a sign-out leaves behind: no credential, rather than a
    // broken one.
    if (cookie_count_ == 0 || cookie_.empty()) {
        return std::unexpected(TokenSourceError::Missing);
    }
    return checked(cookie_);
}

} // namespace infra::auth
