#include "core/models/ids.hpp"

#include "core/errors/domain_error.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <string_view>

namespace core {

namespace {

bool is_subject_char(char c) noexcept {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return true;
    }
    constexpr std::string_view kPunctuation = "._:@|+-";
    return kPunctuation.find(c) != std::string_view::npos;
}

bool is_label_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}

} // namespace

std::expected<UserId, DomainError> UserId::parse(std::string_view text) noexcept {
    if (text.empty()) {
        return std::unexpected(DomainError::EmptyIdentifier);
    }
    if (text.size() > kMaxLength || !std::ranges::all_of(text, is_subject_char)) {
        return std::unexpected(DomainError::MalformedIdentifier);
    }
    UserId id;
    std::ranges::copy(text, id.chars_.begin());
    id.size_ = static_cast<std::uint8_t>(text.size());
    return id;
}

std::expected<NodeId, DomainError> NodeId::parse(std::string_view text) noexcept {
    if (text.empty()) {
        return std::unexpected(DomainError::EmptyIdentifier);
    }
    if (text.size() > kMaxLength || text.front() == '-' || text.back() == '-' ||
        !std::ranges::all_of(text, is_label_char)) {
        return std::unexpected(DomainError::MalformedIdentifier);
    }
    NodeId id;
    std::ranges::copy(text, id.chars_.begin());
    id.size_ = static_cast<std::uint8_t>(text.size());
    return id;
}

} // namespace core
