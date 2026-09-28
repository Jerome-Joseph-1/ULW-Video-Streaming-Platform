#include "core/models/content_type.hpp"

#include <algorithm>

namespace core {

namespace {

// RFC 9110 tchar.
bool is_tchar(char c) noexcept {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return true;
    }
    constexpr std::string_view kExtra = "!#$%&'*+-.^_`|~";
    return kExtra.find(c) != std::string_view::npos;
}

bool is_token(std::string_view s) noexcept {
    return !s.empty() && std::ranges::all_of(s, is_tchar);
}

char to_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

} // namespace

std::expected<ContentType, DomainError> ContentType::parse(std::string_view text) {
    if (text.size() > kMaxLength) {
        return std::unexpected(DomainError::InvalidContentType);
    }
    const std::size_t slash = text.find('/');
    if (slash == std::string_view::npos || !is_token(text.substr(0, slash)) ||
        !is_token(text.substr(slash + 1))) {
        return std::unexpected(DomainError::InvalidContentType);
    }
    std::string lowered(text);
    std::ranges::transform(lowered, lowered.begin(), to_lower);
    return ContentType{std::move(lowered)};
}

} // namespace core
