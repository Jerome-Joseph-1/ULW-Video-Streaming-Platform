#include "core/models/storage_key.hpp"

namespace core {

namespace {

bool is_key_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '_' || c == '-';
}

} // namespace

std::expected<StorageKey, DomainError> StorageKey::parse(std::string_view text) {
    if (text.empty() || text.size() > kMaxLength) {
        return std::unexpected(DomainError::InvalidStorageKey);
    }
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t slash = text.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? text.size() : slash;
        const std::string_view segment = text.substr(start, end - start);
        if (segment.empty() || segment == "." || segment == "..") {
            return std::unexpected(DomainError::InvalidStorageKey);
        }
        for (const char c : segment) {
            if (!is_key_char(c)) {
                return std::unexpected(DomainError::InvalidStorageKey);
            }
        }
        start = end + 1;
    }
    return StorageKey{std::string(text)};
}

} // namespace core
