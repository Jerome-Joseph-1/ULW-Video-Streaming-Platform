#pragma once

#include <algorithm>
#include <string_view>

namespace http::detail {

[[nodiscard]] constexpr char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] constexpr bool iequals(std::string_view a, std::string_view b) noexcept {
    return std::ranges::equal(a, b,
                              [](char x, char y) { return ascii_lower(x) == ascii_lower(y); });
}

} // namespace http::detail
