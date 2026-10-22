#pragma once

#include "core/errors/domain_error.hpp"

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace core {

// The language of a video's captions (ADR-0139): a BCP 47 tag of the plain shape players and
// HLS's LANGUAGE attribute take, a primary subtag of 2 or 3 letters and then subtags of 1 to 8
// letters or digits, each after a '-' ("en", "pt-br", "zh-hant", "es-419"). Tags compare
// without regard to case, so the text form is lowercase: "en-US" and "en-us" are one language,
// one stored file and one path segment. Every character is one a storage key, a path segment
// and a quoted playlist attribute take as it is.
class CaptionLanguage {
public:
    // RFC 5646 section 4.4.1's advice for the longest tag an application should keep.
    static constexpr std::size_t kMaxLength = 35;

    [[nodiscard]] static std::expected<CaptionLanguage, DomainError> parse(std::string_view text);

    [[nodiscard]] std::string_view view() const noexcept { return value_; }
    [[nodiscard]] const std::string& str() const noexcept { return value_; }

    friend bool operator==(const CaptionLanguage&, const CaptionLanguage&) = default;
    friend auto operator<=>(const CaptionLanguage&, const CaptionLanguage&) = default;

private:
    explicit CaptionLanguage(std::string value) : value_(std::move(value)) {}
    std::string value_;
};

} // namespace core
