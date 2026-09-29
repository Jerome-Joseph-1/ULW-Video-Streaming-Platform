#pragma once

#include "libpq_handles.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace infra::postgres {

// A successful statement's result in text format.
class Result {
public:
    explicit Result(ResultHandle handle) noexcept : handle_(std::move(handle)) {}

    [[nodiscard]] int rows() const noexcept { return PQntuples(handle_.get()); }
    // nullopt for SQL NULL and for a cell outside the result. The view lives as long as this.
    [[nodiscard]] std::optional<std::string_view> get(int row, int column) const noexcept;
    // Rows an INSERT, UPDATE or DELETE touched; 0 for any other statement.
    [[nodiscard]] std::uint64_t affected() const noexcept;

private:
    ResultHandle handle_;
};

[[nodiscard]] std::optional<std::int64_t> parse_int64(std::string_view text) noexcept;
[[nodiscard]] std::optional<std::uint64_t> parse_uint64(std::string_view text) noexcept;
// The text form of boolean: "t" or "f".
[[nodiscard]] std::optional<bool> parse_bool(std::string_view text) noexcept;

// A cell holding a domain value with a static `parse` returning std::expected (ids, keys).
template <class T>
[[nodiscard]] std::optional<T> domain_at(const Result& result, int row, int column) {
    const std::optional<std::string_view> text = result.get(row, column);
    if (!text) {
        return std::nullopt;
    }
    auto value = T::parse(*text);
    if (!value) {
        return std::nullopt;
    }
    return std::move(*value);
}

} // namespace infra::postgres
