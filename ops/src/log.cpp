#include "ops/log.hpp"

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <unistd.h>

namespace ops {

namespace {

constexpr std::string_view kTruncated = R"(,"truncated":true)";
// Room kept for the marker and the closing "}\n".
constexpr std::size_t kReserve = kTruncated.size() + 2;

template <std::size_t N> std::string_view digits(std::array<char, N>& out, auto value) noexcept {
    // to_chars takes a [first, last) pointer pair.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const auto [end, ec] = std::to_chars(out.data(), out.data() + out.size(), value);
    if (ec != std::errc{}) {
        return "0";
    }
    return {out.data(), static_cast<std::size_t>(end - out.data())};
}

void pad(std::array<char, 24>& out, std::size_t& at, unsigned value, int width) noexcept {
    for (int i = width - 1; i >= 0; --i) {
        out.at(at + static_cast<std::size_t>(i)) = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    at += static_cast<std::size_t>(width);
}

// 2026-09-29T10:11:12.345Z, without std::format, which may allocate for chrono types.
std::string_view timestamp(std::array<char, 24>& out, core::WallTime at) noexcept {
    using namespace std::chrono;
    const auto ms = time_point_cast<milliseconds>(at);
    const auto day = floor<days>(ms);
    const year_month_day date{day};
    const hh_mm_ss<milliseconds> time{ms - day};
    std::size_t n = 0;
    pad(out, n, static_cast<unsigned>(static_cast<int>(date.year())), 4);
    out.at(n++) = '-';
    pad(out, n, static_cast<unsigned>(date.month()), 2);
    out.at(n++) = '-';
    pad(out, n, static_cast<unsigned>(date.day()), 2);
    out.at(n++) = 'T';
    pad(out, n, static_cast<unsigned>(time.hours().count()), 2);
    out.at(n++) = ':';
    pad(out, n, static_cast<unsigned>(time.minutes().count()), 2);
    out.at(n++) = ':';
    pad(out, n, static_cast<unsigned>(time.seconds().count()), 2);
    out.at(n++) = '.';
    pad(out, n, static_cast<unsigned>(time.subseconds().count()), 3);
    out.at(n++) = 'Z';
    return {out.data(), n};
}

} // namespace

std::optional<Level> parse_level(std::string_view text) noexcept {
    if (text == "debug") {
        return Level::Debug;
    }
    if (text == "info") {
        return Level::Info;
    }
    if (text == "warn") {
        return Level::Warn;
    }
    if (text == "error") {
        return Level::Error;
    }
    return std::nullopt;
}

std::string_view to_string(Level level) noexcept {
    switch (level) {
    case Level::Debug:
        return "debug";
    case Level::Info:
        return "info";
    case Level::Warn:
        return "warn";
    case Level::Error:
        return "error";
    }
    return "error";
}

LineBuilder::LineBuilder(core::WallTime at, Level level, std::string_view service,
                         std::string_view event) noexcept {
    std::array<char, 24> ts{};
    raw(R"({"ts":")");
    raw(timestamp(ts, at));
    raw(R"(","level":")");
    raw(to_string(level));
    raw(R"(","svc":)");
    quoted(service);
    raw(R"(,"event":)");
    quoted(event);
}

bool LineBuilder::fits(std::size_t n) const noexcept {
    return len_ + n + kReserve <= buf_.size();
}

void LineBuilder::raw(std::string_view text) noexcept {
    for (const char c : text) {
        buf_.at(len_++) = c;
    }
}

// JSON string escaping. The worst case is six bytes per input byte (\u001f); room is checked
// against that before anything is written, so a field goes in whole or not at all.
void LineBuilder::quoted(std::string_view text) noexcept {
    constexpr std::string_view kHex = "0123456789abcdef";
    raw("\"");
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        switch (c) {
        case '"':
            raw("\\\"");
            break;
        case '\\':
            raw("\\\\");
            break;
        case '\n':
            raw("\\n");
            break;
        case '\r':
            raw("\\r");
            break;
        case '\t':
            raw("\\t");
            break;
        default:
            if (u < 0x20 || u == 0x7f) {
                raw("\\u00");
                buf_.at(len_++) = kHex[u >> 4U];
                buf_.at(len_++) = kHex[u & 0xFU];
            } else {
                buf_.at(len_++) = c;
            }
        }
    }
    raw("\"");
}

void LineBuilder::add(const Field& field) noexcept {
    if (truncated_) {
        return;
    }
    std::array<char, 32> num{};
    std::string_view text;
    const auto* string = std::get_if<std::string_view>(&field.value());
    if (string != nullptr) {
        text = *string;
    } else if (const auto* b = std::get_if<bool>(&field.value())) {
        text = *b ? "true" : "false";
    } else if (const auto* i = std::get_if<std::int64_t>(&field.value())) {
        text = digits(num, *i);
    } else if (const auto* u = std::get_if<std::uint64_t>(&field.value())) {
        text = digits(num, *u);
    } else if (const auto* d = std::get_if<double>(&field.value())) {
        text = digits(num, *d);
    }
    // ,"key": plus the value, escaped at worst.
    const std::size_t worst =
        4 + (6 * field.key().size()) + (string != nullptr ? 2 + (6 * text.size()) : text.size());
    if (!fits(worst)) {
        truncated_ = true;
        return;
    }
    raw(",");
    quoted(field.key());
    raw(":");
    if (string != nullptr) {
        quoted(text);
    } else {
        raw(text);
    }
}

std::string_view LineBuilder::finish() noexcept {
    if (truncated_) {
        raw(kTruncated);
    }
    raw("}\n");
    return {buf_.data(), len_};
}

void StdoutSink::write(std::string_view line) noexcept {
    // One write per line keeps lines from threads of one process whole: a pipe write of at
    // most PIPE_BUF (4096) bytes is atomic, and a line is at most 1 KiB.
    while (!line.empty()) {
        const ssize_t n = ::write(fd_, line.data(), line.size());
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return;
        }
        line.remove_prefix(static_cast<std::size_t>(n));
    }
}

void Logger::log(Level level, std::string_view event,
                 std::initializer_list<Field> fields) noexcept {
    if (!enabled(level)) {
        return;
    }
    LineBuilder line(clock_.wall_now(), level, service_, event);
    for (const Field& f : fields) {
        line.add(f);
    }
    sink_.write(line.finish());
}

} // namespace ops
