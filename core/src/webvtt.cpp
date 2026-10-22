#include "core/util/webvtt.hpp"

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <string_view>

namespace core::webvtt {

namespace {

constexpr std::string_view kArrow = "-->";
constexpr std::string_view kSignature = "WEBVTT";
constexpr std::string_view kByteOrderMark = "\xEF\xBB\xBF";
// Hours are two or more digits; ten keep a time in milliseconds far inside 64 bits.
constexpr std::size_t kMaxHourDigits = 10;

std::unexpected<Error> fail(Problem problem, std::size_t line) {
    return std::unexpected(Error{.problem = problem, .line = line});
}

[[nodiscard]] bool has_arrow(std::string_view line) noexcept {
    return line.find(kArrow) != std::string_view::npos;
}

[[nodiscard]] bool is_space(char c) noexcept {
    return c == ' ' || c == '\t';
}

[[nodiscard]] bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

[[nodiscard]] unsigned byte_at(std::string_view s, std::size_t i) noexcept {
    return static_cast<unsigned char>(s[i]);
}

// The length of the well-formed UTF-8 sequence at `i` (RFC 3629's table, which leaves out
// overlong forms, surrogates and everything past U+10FFFF), or 0 for none. A NUL is 0 too: the
// parser turns it into U+FFFD, which is not what its author wrote.
[[nodiscard]] std::size_t sequence_length(std::string_view s, std::size_t i) noexcept {
    const unsigned lead = byte_at(s, i);
    if (lead < 0x80) {
        return lead == 0 ? 0 : 1;
    }
    std::size_t length = 0;
    unsigned low = 0x80;
    unsigned high = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        low = lead == 0xE0 ? 0xA0 : low;
        high = lead == 0xED ? 0x9F : high;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        low = lead == 0xF0 ? 0x90 : low;
        high = lead == 0xF4 ? 0x8F : high;
    } else {
        return 0;
    }
    if (s.size() - i < length || byte_at(s, i + 1) < low || byte_at(s, i + 1) > high) {
        return 0;
    }
    for (std::size_t k = 2; k < length; ++k) {
        if (byte_at(s, i + k) < 0x80 || byte_at(s, i + k) > 0xBF) {
            return 0;
        }
    }
    return length;
}

// The line of the first byte that is not UTF-8, or 0 when there is none.
[[nodiscard]] std::size_t misencoded_line(std::string_view text) noexcept {
    std::size_t line = 1;
    for (std::size_t i = 0; i < text.size();) {
        const std::size_t length = sequence_length(text, i);
        if (length == 0) {
            return line;
        }
        // CR LF ends one line, as a lone CR or LF does.
        const bool ends_line =
            text[i] == '\n' || (text[i] == '\r' && (i + 1 == text.size() || text[i + 1] != '\n'));
        line += ends_line ? 1 : 0;
        i += length;
    }
    return 0;
}

// The lines of a text, each without its terminator, and the number of the last one given.
class Lines {
public:
    explicit Lines(std::string_view text) noexcept : rest_(text) {}

    [[nodiscard]] std::optional<std::string_view> next() noexcept {
        if (rest_.empty()) {
            return std::nullopt;
        }
        ++number_;
        const std::size_t end = rest_.find_first_of("\r\n");
        const std::string_view line = rest_.substr(0, end);
        if (end == std::string_view::npos) {
            rest_ = {};
            return line;
        }
        const bool crlf = rest_[end] == '\r' && end + 1 < rest_.size() && rest_[end + 1] == '\n';
        rest_.remove_prefix(end + (crlf ? 2 : 1));
        return line;
    }

    [[nodiscard]] std::size_t number() const noexcept { return number_; }

private:
    std::string_view rest_;
    std::size_t number_ = 0;
};

// The first line: "WEBVTT", alone or then a space or a tab and anything.
[[nodiscard]] bool is_signature(std::string_view line) noexcept {
    return line.starts_with(kSignature) &&
           (line.size() == kSignature.size() || is_space(line[kSignature.size()]));
}

// "NOTE", alone or then a space or a tab and anything: a comment, wherever it stands.
[[nodiscard]] bool is_note(std::string_view line) noexcept {
    constexpr std::string_view kNote = "NOTE";
    return line.starts_with(kNote) && (line.size() == kNote.size() || is_space(line[kNote.size()]));
}

// "STYLE" or "REGION" and nothing but blanks after it.
[[nodiscard]] bool is_definition(std::string_view line) noexcept {
    for (const std::string_view word : {std::string_view{"STYLE"}, std::string_view{"REGION"}}) {
        if (line.starts_with(word) && std::ranges::all_of(line.substr(word.size()), is_space)) {
            return true;
        }
    }
    return false;
}

// Exactly `count` digits at the front of `s`, taken off it.
[[nodiscard]] std::optional<std::uint64_t> digits(std::string_view& s, std::size_t count) {
    if (s.size() < count || !std::ranges::all_of(s.substr(0, count), is_digit)) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : s.substr(0, count)) {
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    s.remove_prefix(count);
    return value;
}

// "[hh:]mm:ss.ttt" at the front of `s`, in milliseconds, taken off it. Hours are two or more
// digits, minutes and seconds two each and below 60, the fraction three.
[[nodiscard]] std::optional<std::uint64_t> timestamp(std::string_view& s) {
    const std::size_t lead =
        static_cast<std::size_t>(std::ranges::find_if_not(s, is_digit) - s.begin());
    std::uint64_t hours = 0;
    if (lead != 2) {
        if (lead < 2 || lead > kMaxHourDigits) {
            return std::nullopt;
        }
        const auto hh = digits(s, lead);
        if (!hh) {
            return std::nullopt;
        }
        hours = *hh;
        if (!s.starts_with(':')) {
            return std::nullopt;
        }
        s.remove_prefix(1);
    }
    auto minutes = digits(s, 2);
    if (!minutes || !s.starts_with(':')) {
        return std::nullopt;
    }
    s.remove_prefix(1);
    auto seconds = digits(s, 2);
    if (!seconds) {
        return std::nullopt;
    }
    // Two pairs of digits so far: with a third pair after them, the first was the hours.
    if (lead == 2 && s.starts_with(':')) {
        s.remove_prefix(1);
        hours = *minutes;
        minutes = seconds;
        seconds = digits(s, 2);
        if (!seconds) {
            return std::nullopt;
        }
    }
    if (!s.starts_with('.')) {
        return std::nullopt;
    }
    s.remove_prefix(1);
    const auto fraction = digits(s, 3);
    if (!fraction || *minutes > 59 || *seconds > 59) {
        return std::nullopt;
    }
    return ((hours * 60 + *minutes) * 60 + *seconds) * 1000 + *fraction;
}

// Blanks at the front of `s`, taken off it; false when there are none.
[[nodiscard]] bool blanks(std::string_view& s) noexcept {
    const std::size_t n =
        static_cast<std::size_t>(std::ranges::find_if_not(s, is_space) - s.begin());
    s.remove_prefix(n);
    return n > 0;
}

struct CueTimes {
    std::uint64_t start = 0;
    std::uint64_t end = 0;
};

// "<start> --> <end>", then the end of the line or blanks and the cue's settings.
[[nodiscard]] std::optional<CueTimes> cue_times(std::string_view line) {
    const auto start = timestamp(line);
    if (!start || !blanks(line) || !line.starts_with(kArrow)) {
        return std::nullopt;
    }
    line.remove_prefix(kArrow.size());
    if (!blanks(line)) {
        return std::nullopt;
    }
    const auto end = timestamp(line);
    if (!end || (!line.empty() && !is_space(line.front()))) {
        return std::nullopt;
    }
    return CueTimes{.start = *start, .end = *end};
}

// Reads the rest of a block, up to a blank line or the end, and answers the line of the first
// that holds "-->", if any does.
[[nodiscard]] std::optional<std::size_t> arrow_in_rest(Lines& lines) noexcept {
    std::optional<std::size_t> found;
    while (const auto line = lines.next()) {
        if (line->empty()) {
            break;
        }
        if (!found && has_arrow(*line)) {
            found = lines.number();
        }
    }
    return found;
}

} // namespace

std::string_view to_string(Problem p) noexcept {
    switch (p) {
    case Problem::Encoding:
        return "encoding";
    case Problem::Header:
        return "header";
    case Problem::Timing:
        return "timing";
    case Problem::Order:
        return "order";
    case Problem::Block:
        return "block";
    }
    return "block";
}

std::expected<void, Error> check(std::string_view text) {
    if (const std::size_t line = misencoded_line(text); line != 0) {
        return fail(Problem::Encoding, line);
    }
    if (text.starts_with(kByteOrderMark)) {
        text.remove_prefix(kByteOrderMark.size());
    }
    Lines lines(text);
    const auto first = lines.next();
    if (!first || !is_signature(*first)) {
        return fail(Problem::Header, 1);
    }
    // Lines under the signature, up to the first blank one, are the header's own (an
    // X-TIMESTAMP-MAP among them), which players skip; one with "-->" would be read as a cue.
    if (const auto at = arrow_in_rest(lines)) {
        return fail(Problem::Header, *at);
    }
    bool seen_cue = false;
    std::uint64_t last_start = 0;
    while (true) {
        auto line = lines.next();
        while (line && line->empty()) {
            line = lines.next();
        }
        if (!line) {
            return {};
        }
        const std::size_t block_line = lines.number();
        // A comment anywhere; a style sheet or a region only ahead of every cue, where players
        // read them. Past the first cue a parser takes such a block for a cue.
        if (is_note(*line) || (!seen_cue && is_definition(*line))) {
            if (has_arrow(*line)) {
                return fail(Problem::Block, block_line);
            }
            if (const auto at = arrow_in_rest(lines)) {
                return fail(Problem::Block, *at);
            }
            continue;
        }
        // A cue: an identifier first unless its first line is the timing.
        std::string_view timing = *line;
        std::size_t timing_line = block_line;
        if (!has_arrow(timing)) {
            const auto second = lines.next();
            if (!second || !has_arrow(*second)) {
                return fail(Problem::Block, block_line);
            }
            timing = *second;
            timing_line = lines.number();
        }
        const auto times = cue_times(timing);
        if (!times || times->end <= times->start) {
            return fail(Problem::Timing, timing_line);
        }
        if (seen_cue && times->start < last_start) {
            return fail(Problem::Order, timing_line);
        }
        seen_cue = true;
        last_start = times->start;
        if (const auto at = arrow_in_rest(lines)) {
            return fail(Problem::Block, *at);
        }
    }
}

} // namespace core::webvtt
