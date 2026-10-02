#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace core::json {

// Strict RFC 8259. Duplicate object keys are rejected rather than resolved: tokens and request
// bodies are read by more than one parser, and "last one wins" in one of them is a known way to
// smuggle a second value past the other.
class Value {
public:
    enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };
    using Member = std::pair<std::string, Value>;

    [[nodiscard]] Kind kind() const noexcept { return kind_; }
    [[nodiscard]] bool is_null() const noexcept { return kind_ == Kind::Null; }

    [[nodiscard]] std::optional<bool> as_bool() const noexcept;
    // Integers only: a fraction, exponent, sign or out-of-range value is nullopt.
    [[nodiscard]] std::optional<std::uint64_t> as_u64() const noexcept;
    [[nodiscard]] std::optional<std::int64_t> as_i64() const noexcept;
    [[nodiscard]] std::optional<std::string_view> as_string() const noexcept;
    [[nodiscard]] const std::vector<Value>* as_array() const noexcept;
    [[nodiscard]] const std::vector<Member>* as_object() const noexcept;

    // Member lookup on an object; nullptr when absent or when this is not an object.
    [[nodiscard]] const Value* find(std::string_view key) const noexcept;

private:
    friend class Parser;

    Kind kind_ = Kind::Null;
    bool boolean_ = false;
    // Numbers keep their source text so integer and floating reads each parse it exactly once.
    std::string text_;
    std::vector<Value> items_;
    std::vector<Member> members_;
};

// `offset` is where parsing stopped: for a duplicate key, the end of the key that repeats an
// earlier one in the same object. Duplicates are looked for when the object closes, so another
// error later in that object (a bad value, a missing '}') is the one reported.
struct ParseError {
    std::size_t offset;
    std::string_view reason;
};

struct Limits {
    // Nothing this service reads nests deeper than 4; 32 leaves room while bounding recursion.
    std::size_t max_depth = 32;
    std::size_t max_bytes = std::size_t{64} * 1024;
};

[[nodiscard]] std::expected<Value, ParseError> parse(std::string_view text, Limits limits = {});

// Appends `s` to `out` as a JSON string literal, quotes included.
void append_string(std::string& out, std::string_view s);

} // namespace core::json
