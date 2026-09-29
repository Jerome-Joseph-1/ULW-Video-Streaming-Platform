#include "result.hpp"

#include <charconv>
#include <system_error>

namespace infra::postgres {

namespace {

template <class T> std::optional<T> parse_whole(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    T value{};
    const char* const first = std::to_address(text.begin());
    const char* const last = std::to_address(text.end());
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::uint8_t> hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return static_cast<std::uint8_t>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<std::uint8_t>(c - 'a' + 10);
    }
    return std::nullopt;
}

} // namespace

std::optional<std::string_view> Result::get(int row, int column) const noexcept {
    const PGresult* res = handle_.get();
    if (PQgetisnull(res, row, column) != 0) {
        return std::nullopt;
    }
    return std::string_view{PQgetvalue(res, row, column),
                            static_cast<std::size_t>(PQgetlength(res, row, column))};
}

std::uint64_t Result::affected() const noexcept {
    return parse_uint64(PQcmdTuples(handle_.get())).value_or(0);
}

std::optional<std::int64_t> parse_int64(std::string_view text) noexcept {
    return parse_whole<std::int64_t>(text);
}

std::optional<std::uint64_t> parse_uint64(std::string_view text) noexcept {
    return parse_whole<std::uint64_t>(text);
}

std::optional<bool> parse_bool(std::string_view text) noexcept {
    if (text == "t") {
        return true;
    }
    if (text == "f") {
        return false;
    }
    return std::nullopt;
}

std::optional<std::vector<std::byte>> parse_hex(std::string_view text) {
    if (text.size() % 2 != 0) {
        return std::nullopt;
    }
    std::vector<std::byte> out(text.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto high = hex_digit(text[2 * i]);
        const auto low = hex_digit(text[(2 * i) + 1]);
        if (!high || !low) {
            return std::nullopt;
        }
        out[i] = static_cast<std::byte>((*high << 4U) | *low);
    }
    return out;
}

} // namespace infra::postgres
