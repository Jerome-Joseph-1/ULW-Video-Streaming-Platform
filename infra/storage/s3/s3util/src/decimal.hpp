#pragma once

#include <charconv>
#include <concepts>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>

namespace infra::s3util::detail {

// Digits only: no sign, no whitespace, no trailing junk, no overflow.
template <std::unsigned_integral T>
[[nodiscard]] std::optional<T> parse_decimal(std::string_view text) noexcept {
    T value{};
    const char* const end = std::to_address(text.end());
    const auto [ptr, ec] = std::from_chars(std::to_address(text.begin()), end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

} // namespace infra::s3util::detail
