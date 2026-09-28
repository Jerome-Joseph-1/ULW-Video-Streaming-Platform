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

} // namespace infra::postgres
