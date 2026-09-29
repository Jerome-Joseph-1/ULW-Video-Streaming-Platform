#include "ops/toml.hpp"

#include "core/util/parse.hpp"

#include <array>
#include <set>
#include <utility>

namespace ops::toml {

namespace {

using Failure = std::unexpected<std::string>;

bool is_bare(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-';
}

bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

// Tab is the only control character TOML allows in a string or a comment.
bool is_forbidden_control(char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    return (u < 0x20 && c != '\t') || u == 0x7f;
}

// TOML documents are UTF-8; so are the strings a caller gets back.
bool valid_utf8(std::string_view text) noexcept {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t extra = 0;
        std::uint32_t cp = 0;
        if (lead < 0x80) {
            ++i;
            continue;
        }
        if ((lead & 0xE0U) == 0xC0) {
            extra = 1;
            cp = lead & 0x1FU;
        } else if ((lead & 0xF0U) == 0xE0) {
            extra = 2;
            cp = lead & 0x0FU;
        } else if ((lead & 0xF8U) == 0xF0) {
            extra = 3;
            cp = lead & 0x07U;
        } else {
            return false;
        }
        if (extra >= text.size() - i) {
            return false;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto cont = static_cast<unsigned char>(text[i + k]);
            if ((cont & 0xC0U) != 0x80) {
                return false;
            }
            cp = (cp << 6U) | (cont & 0x3FU);
        }
        // Overlong forms, surrogates and values past U+10FFFF.
        constexpr std::array<std::uint32_t, 4> kMinimum{0, 0x80, 0x800, 0x10000};
        if (cp < kMinimum.at(extra) || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0U | (cp >> 6U));
        out += static_cast<char>(0x80U | (cp & 0x3FU));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0U | (cp >> 12U));
        out += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
        out += static_cast<char>(0x80U | (cp & 0x3FU));
    } else {
        out += static_cast<char>(0xF0U | (cp >> 18U));
        out += static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU));
        out += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
        out += static_cast<char>(0x80U | (cp & 0x3FU));
    }
}

class Cursor {
public:
    explicit Cursor(std::string_view line) noexcept : line_(line) {}

    void skip_blank() noexcept {
        while (pos_ < line_.size() && (line_[pos_] == ' ' || line_[pos_] == '\t')) {
            ++pos_;
        }
    }
    [[nodiscard]] bool at_end() const noexcept { return pos_ >= line_.size(); }
    [[nodiscard]] char peek() const noexcept { return at_end() ? '\0' : line_[pos_]; }
    [[nodiscard]] std::string_view rest() const noexcept { return line_.substr(pos_); }
    void advance(std::size_t n = 1) noexcept { pos_ += n; }
    char take() noexcept { return line_[pos_++]; }

    // Nothing but blanks and a comment may follow a statement.
    [[nodiscard]] std::expected<void, std::string> expect_end() noexcept {
        skip_blank();
        if (at_end()) {
            return {};
        }
        if (peek() != '#') {
            return Failure("unexpected text after the value");
        }
        for (const char c : rest()) {
            if (is_forbidden_control(c)) {
                return Failure("control character in a comment");
            }
        }
        pos_ = line_.size();
        return {};
    }

private:
    std::string_view line_;
    std::size_t pos_ = 0;
};

std::expected<std::string, std::string> parse_key(Cursor& c) {
    std::string key;
    while (true) {
        c.skip_blank();
        if (c.peek() == '"' || c.peek() == '\'') {
            return Failure("quoted keys are not supported");
        }
        const std::size_t before = key.size();
        while (!c.at_end() && is_bare(c.peek())) {
            key += c.take();
        }
        if (key.size() == before) {
            return Failure("expected a key");
        }
        c.skip_blank();
        if (c.peek() != '.') {
            return key;
        }
        c.advance();
        key += '.';
    }
}

std::expected<std::uint32_t, std::string> parse_hex(Cursor& c, std::size_t digits) {
    std::uint32_t cp = 0;
    for (std::size_t i = 0; i < digits; ++i) {
        const char h = c.peek();
        std::uint32_t v = 0;
        if (h >= '0' && h <= '9') {
            v = static_cast<std::uint32_t>(h - '0');
        } else if (h >= 'a' && h <= 'f') {
            v = static_cast<std::uint32_t>(h - 'a' + 10);
        } else if (h >= 'A' && h <= 'F') {
            v = static_cast<std::uint32_t>(h - 'A' + 10);
        } else {
            return Failure("bad unicode escape");
        }
        c.advance();
        cp = (cp << 4U) | v;
    }
    if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
        return Failure("escape is not a unicode scalar value");
    }
    return cp;
}

std::expected<std::string, std::string> parse_basic_string(Cursor& c) {
    c.advance();
    std::string out;
    while (true) {
        if (c.at_end()) {
            return Failure("unterminated string");
        }
        const char ch = c.take();
        if (ch == '"') {
            return out;
        }
        if (is_forbidden_control(ch)) {
            return Failure("control character in a string");
        }
        if (ch != '\\') {
            out += ch;
            continue;
        }
        if (c.at_end()) {
            return Failure("unterminated string");
        }
        const char escape = c.take();
        switch (escape) {
        case 'b':
            out += '\b';
            break;
        case 't':
            out += '\t';
            break;
        case 'n':
            out += '\n';
            break;
        case 'f':
            out += '\f';
            break;
        case 'r':
            out += '\r';
            break;
        case '"':
            out += '"';
            break;
        case '\\':
            out += '\\';
            break;
        case 'u':
        case 'U': {
            const auto cp = parse_hex(c, escape == 'u' ? 4 : 8);
            if (!cp) {
                return Failure(cp.error());
            }
            append_utf8(out, *cp);
            break;
        }
        default:
            return Failure("unknown escape in a string");
        }
    }
}

std::expected<std::string, std::string> parse_literal_string(Cursor& c) {
    c.advance();
    std::string out;
    while (true) {
        if (c.at_end()) {
            return Failure("unterminated string");
        }
        const char ch = c.take();
        if (ch == '\'') {
            return out;
        }
        if (is_forbidden_control(ch)) {
            return Failure("control character in a string");
        }
        out += ch;
    }
}

// Decimal only, with single underscores between digits and no leading zero, as TOML has it.
std::expected<std::string, std::string> parse_integer(Cursor& c) {
    std::string token;
    while (!c.at_end() && c.peek() != ' ' && c.peek() != '\t' && c.peek() != '#') {
        token += c.take();
    }
    std::string_view digits = token;
    bool negative = false;
    if (digits.starts_with('+') || digits.starts_with('-')) {
        negative = digits.front() == '-';
        digits.remove_prefix(1);
    }
    if (digits.empty() || !is_digit(digits.front())) {
        return Failure("not a supported value");
    }
    std::string plain = negative ? "-" : "";
    for (std::size_t i = 0; i < digits.size(); ++i) {
        const char d = digits[i];
        if (d == '_' && i > 0 && i + 1 < digits.size() && is_digit(digits[i - 1]) &&
            is_digit(digits[i + 1])) {
            continue;
        }
        if (!is_digit(d)) {
            return Failure("only decimal integers, strings and booleans are supported");
        }
        plain += d;
    }
    const std::string_view magnitude = std::string_view(plain).substr(negative ? 1 : 0);
    if (magnitude.size() > 1 && magnitude.front() == '0') {
        return Failure("leading zero in an integer");
    }
    if (!core::parse_integer<std::int64_t>(plain)) {
        return Failure("integer out of range");
    }
    if (plain == "-0") {
        plain = "0";
    }
    return plain;
}

struct Value {
    Kind kind;
    std::string text;
};

std::expected<Value, std::string> parse_value(Cursor& c) {
    c.skip_blank();
    const std::string_view rest = c.rest();
    if (rest.starts_with(R"(""")") || rest.starts_with("'''")) {
        return Failure("multi-line strings are not supported");
    }
    if (rest.starts_with('"')) {
        return parse_basic_string(c).transform(
            [](std::string s) { return Value{.kind = Kind::String, .text = std::move(s)}; });
    }
    if (rest.starts_with('\'')) {
        return parse_literal_string(c).transform(
            [](std::string s) { return Value{.kind = Kind::String, .text = std::move(s)}; });
    }
    for (const std::string_view word : {"true", "false"}) {
        if (rest.starts_with(word)) {
            const std::string_view after = rest.substr(word.size());
            if (after.empty() || after.front() == ' ' || after.front() == '\t' ||
                after.front() == '#') {
                c.advance(word.size());
                return Value{.kind = Kind::Boolean, .text = std::string(word)};
            }
        }
    }
    if (rest.starts_with('[')) {
        return Failure("arrays are not supported");
    }
    if (rest.starts_with('{')) {
        return Failure("inline tables are not supported");
    }
    if (rest.empty()) {
        return Failure("expected a value");
    }
    return parse_integer(c).transform(
        [](std::string s) { return Value{.kind = Kind::Integer, .text = std::move(s)}; });
}

class Document {
public:
    std::expected<void, std::string> open_table(const std::string& name) {
        if (tables_.contains(name)) {
            return Failure("table [" + name + "] defined twice");
        }
        if (auto r = check_prefixes(name); !r) {
            return r;
        }
        if (values_.contains(name)) {
            return Failure(name + " is already a value");
        }
        tables_.insert(name);
        current_ = name;
        return {};
    }

    std::expected<void, std::string> add(const std::string& key, Value value, std::size_t line) {
        std::string full = current_.empty() ? key : current_ + "." + key;
        if (values_.contains(full)) {
            return Failure(full + " defined twice");
        }
        if (auto r = check_prefixes(full); !r) {
            return r;
        }
        // A value may not take a name some table, or some longer key, already lives under.
        const std::string below = full + ".";
        const auto it = values_.lower_bound(below);
        if (tables_.contains(full) || (it != values_.end() && it->starts_with(below))) {
            return Failure(full + " is already a table");
        }
        values_.insert(full);
        entries_.push_back({.key = std::move(full),
                            .kind = value.kind,
                            .value = std::move(value.text),
                            .line = line});
        return {};
    }

    std::vector<Entry> take() { return std::move(entries_); }

private:
    // No proper prefix of `name` may be a value: it would have to be a table too.
    [[nodiscard]] std::expected<void, std::string> check_prefixes(const std::string& name) const {
        for (std::size_t dot = name.find('.'); dot != std::string::npos;
             dot = name.find('.', dot + 1)) {
            const std::string prefix = name.substr(0, dot);
            if (values_.contains(prefix)) {
                return Failure(prefix + " is a value, not a table");
            }
        }
        return {};
    }

    std::set<std::string> values_;
    std::set<std::string> tables_;
    std::string current_;
    std::vector<Entry> entries_;
};

std::expected<void, std::string> parse_line(std::string_view text, std::size_t line,
                                            Document& doc) {
    Cursor c(text);
    c.skip_blank();
    if (c.at_end() || c.peek() == '#') {
        return c.expect_end();
    }
    if (c.peek() == '[') {
        c.advance();
        if (c.peek() == '[') {
            return Failure("arrays of tables are not supported");
        }
        auto name = parse_key(c);
        if (!name) {
            return Failure(std::move(name.error()));
        }
        c.skip_blank();
        if (c.peek() != ']') {
            return Failure("expected ] after the table name");
        }
        c.advance();
        if (auto r = c.expect_end(); !r) {
            return r;
        }
        return doc.open_table(*name);
    }
    auto key = parse_key(c);
    if (!key) {
        return Failure(std::move(key.error()));
    }
    c.skip_blank();
    if (c.peek() != '=') {
        return Failure("expected = after the key");
    }
    c.advance();
    auto value = parse_value(c);
    if (!value) {
        return Failure(std::move(value.error()));
    }
    if (auto r = c.expect_end(); !r) {
        return r;
    }
    return doc.add(*key, std::move(*value), line);
}

} // namespace

std::expected<std::vector<Entry>, ParseError> parse(std::string_view document) {
    if (!valid_utf8(document)) {
        return std::unexpected(ParseError{.line = 0, .reason = "not UTF-8"});
    }
    Document doc;
    std::size_t line = 0;
    while (!document.empty()) {
        ++line;
        const std::size_t nl = document.find('\n');
        std::string_view text = document.substr(0, nl);
        document = nl == std::string_view::npos ? std::string_view{} : document.substr(nl + 1);
        if (text.ends_with('\r')) {
            text.remove_suffix(1);
        }
        if (auto r = parse_line(text, line, doc); !r) {
            return std::unexpected(ParseError{.line = line, .reason = std::move(r.error())});
        }
    }
    return doc.take();
}

} // namespace ops::toml
