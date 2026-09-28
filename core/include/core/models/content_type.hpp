#pragma once

#include "core/errors/domain_error.hpp"

#include <expected>
#include <string>
#include <string_view>

namespace core {

// A bare media type ("type/subtype"), lowercased. Parameters are rejected: nothing
// downstream consumes them and they are a common injection vector into signed headers.
// Lowercasing canonicalises, not sanitises: RFC 9110 8.3.1 makes type and subtype caseless.
class ContentType {
public:
    static constexpr std::size_t kMaxLength = 127;

    [[nodiscard]] static std::expected<ContentType, DomainError> parse(std::string_view text);

    [[nodiscard]] std::string_view view() const noexcept { return value_; }

    friend bool operator==(const ContentType&, const ContentType&) = default;

private:
    explicit ContentType(std::string value) : value_(std::move(value)) {}
    std::string value_;
};

} // namespace core
