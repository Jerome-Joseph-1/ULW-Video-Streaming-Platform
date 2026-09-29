#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace ops::toml {

enum class Kind : std::uint8_t { String, Integer, Boolean };

struct Entry {
    // Dotted and qualified by its table: `port = 1` under `[listen]` is "listen.port".
    std::string key;
    Kind kind = Kind::String;
    // The string's contents, the integer in plain decimal, or "true" / "false".
    std::string value;
    std::size_t line = 0;
};

struct ParseError {
    std::size_t line = 0;
    std::string reason;
};

// The part of TOML 1.0 a configuration file of scalars needs: comments, [tables] and dotted
// bare keys, basic and literal one-line strings, decimal integers and booleans. Anything else
// the language has (arrays, inline tables, floats, dates, multi-line strings, quoted keys) is
// refused with its line number rather than half-read, as are duplicate keys and tables.
[[nodiscard]] std::expected<std::vector<Entry>, ParseError> parse(std::string_view document);

} // namespace ops::toml
