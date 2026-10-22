#include "core/models/caption_language.hpp"

#include <algorithm>

namespace core {

namespace {

[[nodiscard]] bool is_letter(char c) noexcept {
    return c >= 'a' && c <= 'z';
}

[[nodiscard]] bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

// The primary subtag: 2 or 3 letters. Any after it: 1 to 8 letters or digits.
[[nodiscard]] bool subtag_fits(std::string_view subtag, bool primary) noexcept {
    if (primary) {
        return subtag.size() >= 2 && subtag.size() <= 3 && std::ranges::all_of(subtag, is_letter);
    }
    return !subtag.empty() && subtag.size() <= 8 &&
           std::ranges::all_of(subtag, [](char c) { return is_letter(c) || is_digit(c); });
}

[[nodiscard]] char lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

} // namespace

std::expected<CaptionLanguage, DomainError> CaptionLanguage::parse(std::string_view text) {
    if (text.empty()) {
        return std::unexpected(DomainError::EmptyIdentifier);
    }
    if (text.size() > kMaxLength) {
        return std::unexpected(DomainError::MalformedIdentifier);
    }
    std::string value(text.size(), '\0');
    std::ranges::transform(text, value.begin(), lower);
    std::string_view rest = value;
    bool primary = true;
    while (true) {
        const std::size_t dash = rest.find('-');
        const std::string_view subtag = rest.substr(0, dash);
        if (!subtag_fits(subtag, primary)) {
            return std::unexpected(DomainError::MalformedIdentifier);
        }
        if (dash == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(dash + 1);
        primary = false;
    }
    return CaptionLanguage{std::move(value)};
}

} // namespace core
