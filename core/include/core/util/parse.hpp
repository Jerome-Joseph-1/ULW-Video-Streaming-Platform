#pragma once

#include <charconv>
#include <concepts>
#include <optional>
#include <string_view>

namespace core {

// A decimal integer that is the whole of `text`: no sign on unsigned types, no whitespace, no
// trailing junk, no overflow. Untrusted numbers go through here, never atoi/stoi.
// from_chars already refuses a leading '+', and '-' for unsigned types; checking that the
// whole text was consumed rules out the rest.
template <std::integral T>
[[nodiscard]] constexpr std::optional<T> parse_integer(std::string_view text) noexcept {
    T value{};
    const char* const first = text.data();
    // from_chars takes a [first, last) pointer pair.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const char* const last = first + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last) {
        return std::nullopt;
    }
    return value;
}

} // namespace core
