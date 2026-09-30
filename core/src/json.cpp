#include "core/util/json.hpp"

#include "core/util/parse.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace core::json {

namespace {

bool is_ws(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Sorted, so that n keys cost n log n comparisons: one lookup per key as it arrives would cost
// n^2 / 2, which for the 7,000 keys a 64 KiB chat command holds is a tenth of a second of the
// reactor thread per message.
bool has_duplicate_key(const std::vector<Value::Member>& members) {
    std::vector<std::string_view> keys;
    keys.reserve(members.size());
    for (const auto& [key, value] : members) {
        keys.emplace_back(key);
    }
    std::ranges::sort(keys);
    return std::ranges::adjacent_find(keys) != keys.end();
}

std::optional<unsigned> hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return static_cast<unsigned>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<unsigned>(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<unsigned>(c - 'A' + 10);
    }
    return std::nullopt;
}

void append_utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Length of the well-formed UTF-8 sequence at `s`, or 0. Rows are Unicode 15 table 3-7: the
// lead byte range, the sequence length, and the range the second byte must fall in (it is what
// excludes overlong forms, surrogates and code points above U+10FFFF).
std::size_t utf8_length(std::string_view s) noexcept {
    struct Row {
        unsigned char lead_lo, lead_hi;
        std::size_t length;
        unsigned char second_lo, second_hi;
    };
    // Positional rows read as the table they transcribe.
    // NOLINTBEGIN(modernize-use-designated-initializers)
    constexpr std::array<Row, 8> kRows{{
        {0xC2, 0xDF, 2, 0x80, 0xBF},
        {0xE0, 0xE0, 3, 0xA0, 0xBF},
        {0xE1, 0xEC, 3, 0x80, 0xBF},
        {0xED, 0xED, 3, 0x80, 0x9F},
        {0xEE, 0xEF, 3, 0x80, 0xBF},
        {0xF0, 0xF0, 4, 0x90, 0xBF},
        {0xF1, 0xF3, 4, 0x80, 0xBF},
        {0xF4, 0xF4, 4, 0x80, 0x8F},
    }};
    // NOLINTEND(modernize-use-designated-initializers)
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(s[i]); };
    if (byte(0) < 0x80) {
        return 1;
    }
    for (const Row& r : kRows) {
        if (byte(0) < r.lead_lo || byte(0) > r.lead_hi) {
            continue;
        }
        if (s.size() < r.length || byte(1) < r.second_lo || byte(1) > r.second_hi) {
            return 0;
        }
        for (std::size_t i = 2; i < r.length; ++i) {
            if (byte(i) < 0x80 || byte(i) > 0xBF) {
                return 0;
            }
        }
        return r.length;
    }
    return 0;
}

} // namespace

class Parser {
public:
    Parser(std::string_view text, Limits limits) : text_(text), limits_(limits) {}

    std::expected<Value, ParseError> run() {
        if (text_.size() > limits_.max_bytes) {
            return fail("document too large");
        }
        Value v;
        if (!value(v, 0)) {
            return std::unexpected(error_);
        }
        skip_ws();
        if (pos_ != text_.size()) {
            return fail("trailing data");
        }
        return v;
    }

private:
    std::unexpected<ParseError> fail(std::string_view reason) {
        error_ = {.offset = pos_, .reason = reason};
        return std::unexpected(error_);
    }

    bool set_error(std::string_view reason) {
        error_ = {.offset = pos_, .reason = reason};
        return false;
    }

    void skip_ws() noexcept {
        while (pos_ < text_.size() && is_ws(text_[pos_])) {
            ++pos_;
        }
    }

    bool literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) {
            return set_error("invalid literal");
        }
        pos_ += word.size();
        return true;
    }

    // Recursion is bounded by limits_.max_depth, checked on entry.
    // NOLINTNEXTLINE(misc-no-recursion)
    bool value(Value& out, std::size_t depth) {
        if (depth >= limits_.max_depth) {
            return set_error("nesting too deep");
        }
        skip_ws();
        if (pos_ >= text_.size()) {
            return set_error("unexpected end");
        }
        switch (text_[pos_]) {
        case 'n':
            out.kind_ = Value::Kind::Null;
            return literal("null");
        case 't':
            out.kind_ = Value::Kind::Bool;
            out.boolean_ = true;
            return literal("true");
        case 'f':
            out.kind_ = Value::Kind::Bool;
            out.boolean_ = false;
            return literal("false");
        case '"':
            out.kind_ = Value::Kind::String;
            return string(out.text_);
        case '[':
            return array(out, depth);
        case '{':
            return object(out, depth);
        default:
            return number(out);
        }
    }

    bool number(Value& out) {
        const std::size_t start = pos_;
        const auto digits = [&] {
            const std::size_t from = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
                ++pos_;
            }
            return pos_ - from;
        };
        if (pos_ < text_.size() && text_[pos_] == '-') {
            ++pos_;
        }
        if (pos_ < text_.size() && text_[pos_] == '0') {
            ++pos_;
        } else if (digits() == 0) {
            return set_error("invalid number");
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            if (digits() == 0) {
                return set_error("invalid number");
            }
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
                ++pos_;
            }
            if (digits() == 0) {
                return set_error("invalid number");
            }
        }
        out.kind_ = Value::Kind::Number;
        out.text_.assign(text_.substr(start, pos_ - start));
        return true;
    }

    bool hex4(unsigned& out) {
        if (text_.size() - pos_ < 4) {
            return set_error("truncated escape");
        }
        out = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const auto d = hex_digit(text_[pos_ + i]);
            if (!d) {
                return set_error("invalid escape");
            }
            out = (out << 4U) | *d;
        }
        pos_ += 4;
        return true;
    }

    bool string(std::string& out) {
        ++pos_;
        for (;;) {
            if (pos_ >= text_.size()) {
                return set_error("unterminated string");
            }
            const char c = text_[pos_];
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return set_error("control character in string");
            }
            if (c == '\\') {
                ++pos_;
                if (!escape(out)) {
                    return false;
                }
                continue;
            }
            const std::size_t n = utf8_length(text_.substr(pos_));
            if (n == 0) {
                return set_error("invalid utf-8");
            }
            out.append(text_.substr(pos_, n));
            pos_ += n;
        }
    }

    bool escape(std::string& out) {
        if (pos_ >= text_.size()) {
            return set_error("unterminated string");
        }
        const char e = text_[pos_++];
        constexpr std::string_view kFrom = "\"\\/bfnrt";
        constexpr std::string_view kTo = "\"\\/\b\f\n\r\t";
        if (const auto i = kFrom.find(e); i != std::string_view::npos) {
            out.push_back(kTo[i]);
            return true;
        }
        if (e != 'u') {
            return set_error("invalid escape");
        }
        unsigned hi = 0;
        if (!hex4(hi)) {
            return false;
        }
        char32_t cp = hi;
        if (hi >= 0xDC00 && hi <= 0xDFFF) {
            return set_error("lone surrogate");
        }
        if (hi >= 0xD800 && hi <= 0xDBFF) {
            unsigned lo = 0;
            if (text_.substr(pos_, 2) != "\\u") {
                return set_error("lone surrogate");
            }
            pos_ += 2;
            if (!hex4(lo)) {
                return false;
            }
            if (lo < 0xDC00 || lo > 0xDFFF) {
                return set_error("lone surrogate");
            }
            cp = 0x10000 + (((hi - 0xD800) << 10U) | (lo - 0xDC00));
        }
        append_utf8(out, cp);
        return true;
    }

    bool array(Value& out, std::size_t depth) { // NOLINT(misc-no-recursion): see value()
        out.kind_ = Value::Kind::Array;
        ++pos_;
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            Value item;
            if (!value(item, depth + 1)) {
                return false;
            }
            out.items_.push_back(std::move(item));
            skip_ws();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < text_.size() && text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return set_error("expected ',' or ']'");
        }
    }

    bool object(Value& out, std::size_t depth) { // NOLINT(misc-no-recursion): see value()
        out.kind_ = Value::Kind::Object;
        ++pos_;
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                return set_error("expected key");
            }
            std::string key;
            if (!string(key)) {
                return false;
            }
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                return set_error("expected ':'");
            }
            ++pos_;
            Value v;
            if (!value(v, depth + 1)) {
                return false;
            }
            out.members_.emplace_back(std::move(key), std::move(v));
            skip_ws();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < text_.size() && text_[pos_] == '}') {
                // Checked once the members are all in place, where their keys stay put.
                if (has_duplicate_key(out.members_)) {
                    return set_error("duplicate key");
                }
                ++pos_;
                return true;
            }
            return set_error("expected ',' or '}'");
        }
    }

    std::string_view text_;
    Limits limits_;
    std::size_t pos_ = 0;
    ParseError error_{.offset = 0, .reason = {}};
};

std::expected<Value, ParseError> parse(std::string_view text, Limits limits) {
    return Parser(text, limits).run();
}

std::optional<bool> Value::as_bool() const noexcept {
    return kind_ == Kind::Bool ? std::optional<bool>(boolean_) : std::nullopt;
}

std::optional<std::uint64_t> Value::as_u64() const noexcept {
    return kind_ == Kind::Number ? parse_integer<std::uint64_t>(text_) : std::nullopt;
}

std::optional<std::int64_t> Value::as_i64() const noexcept {
    return kind_ == Kind::Number ? parse_integer<std::int64_t>(text_) : std::nullopt;
}

std::optional<std::string_view> Value::as_string() const noexcept {
    return kind_ == Kind::String ? std::optional<std::string_view>(text_) : std::nullopt;
}

const std::vector<Value>* Value::as_array() const noexcept {
    return kind_ == Kind::Array ? &items_ : nullptr;
}

const std::vector<Value::Member>* Value::as_object() const noexcept {
    return kind_ == Kind::Object ? &members_ : nullptr;
}

const Value* Value::find(std::string_view key) const noexcept {
    if (kind_ != Kind::Object) {
        return nullptr;
    }
    for (const auto& [k, v] : members_) {
        if (k == key) {
            return &v;
        }
    }
    return nullptr;
}

void append_string(std::string& out, std::string_view s) {
    constexpr std::string_view kHex = "0123456789abcdef";
    out.push_back('"');
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        switch (c) {
        case '"':
            out.append("\\\"");
            break;
        case '\\':
            out.append("\\\\");
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\r':
            out.append("\\r");
            break;
        case '\t':
            out.append("\\t");
            break;
        default:
            if (u < 0x20) {
                const std::array<char, 6> esc{'\\', 'u', '0', '0', kHex[u >> 4U], kHex[u & 0xFU]};
                out.append(esc.data(), esc.size());
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
}

} // namespace core::json
