#pragma once

#include "core/errors/domain_error.hpp"

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace core {

// An object key accepted by every backend unchanged: '/'-separated segments of
// [A-Za-z0-9._-], none empty, none "." or "..". Anything else is rejected, never rewritten,
// so a key read back from storage is byte-identical to the one written.
class StorageKey {
public:
    // S3 allows 1024 bytes of UTF-8; ASCII-only keys make bytes and characters agree.
    static constexpr std::size_t kMaxLength = 1024;

    [[nodiscard]] static std::expected<StorageKey, DomainError> parse(std::string_view text);

    [[nodiscard]] std::string_view view() const noexcept { return value_; }
    [[nodiscard]] const std::string& str() const noexcept { return value_; }

    friend bool operator==(const StorageKey&, const StorageKey&) = default;
    friend auto operator<=>(const StorageKey&, const StorageKey&) = default;

private:
    explicit StorageKey(std::string value) : value_(std::move(value)) {}
    std::string value_;
};

} // namespace core
