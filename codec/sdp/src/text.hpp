#pragma once

#include "core/util/parse.hpp"

#include <algorithm>
#include <concepts>
#include <optional>
#include <string_view>

namespace codec::sdp::detail {

// RFC 8866 section 9, token-char.
[[nodiscard]] constexpr bool is_token_char(char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    return u == 0x21 || (u >= 0x23 && u <= 0x27) || u == 0x2A || u == 0x2B || u == 0x2D ||
           u == 0x2E || (u >= 0x30 && u <= 0x39) || (u >= 0x41 && u <= 0x5A) ||
           (u >= 0x5E && u <= 0x7E);
}

[[nodiscard]] constexpr bool is_token(std::string_view s) noexcept {
    return !s.empty() && std::ranges::all_of(s, is_token_char);
}

// RFC 8866 section 9, non-ws-string: visible ASCII and any byte from 0x80.
[[nodiscard]] constexpr bool is_non_ws_string(std::string_view s) noexcept {
    return !s.empty() && std::ranges::all_of(s, [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u > 0x20 && u != 0x7F;
    });
}

// A decimal number with no leading zero. The grammars allow some (sess-id is 1*DIGIT), but
// no implementation sends one, and refusing them is what lets a number serialize back to the
// text it came from.
template <std::unsigned_integral T>
[[nodiscard]] constexpr std::optional<T> canonical_number(std::string_view s) noexcept {
    if (s.size() > 1 && s.front() == '0') {
        return std::nullopt;
    }
    return core::parse_integer<T>(s);
}

// Splits on single spaces. Two spaces in a row, or a leading or trailing one, yield an empty
// field, which no grammar here allows: callers refuse empty fields.
class Fields {
public:
    explicit Fields(std::string_view text) noexcept : rest_(text) {}

    [[nodiscard]] bool done() const noexcept { return done_; }

    [[nodiscard]] std::string_view next() noexcept {
        if (done_) {
            return {};
        }
        const std::size_t space = rest_.find(' ');
        if (space == std::string_view::npos) {
            done_ = true;
            return rest_;
        }
        const std::string_view field = rest_.substr(0, space);
        rest_.remove_prefix(space + 1);
        return field;
    }

    // What next() has not returned yet, spaces and all.
    [[nodiscard]] std::string_view rest() const noexcept {
        return done_ ? std::string_view{} : rest_;
    }

private:
    std::string_view rest_;
    bool done_ = false;
};

} // namespace codec::sdp::detail
