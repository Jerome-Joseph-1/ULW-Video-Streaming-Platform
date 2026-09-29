#pragma once

#include "codec/sdp/error.hpp"
#include "codec/sdp/parser.hpp"
#include "codec/sdp/session.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace codec::sdp {

// For gtest's failure messages.
inline void PrintTo(const Error& e, std::ostream* os) {
    *os << "line " << e.line << ": " << message(e.code);
}

} // namespace codec::sdp

namespace ulw::test {

using Lines = std::vector<std::string>;

// 32 bytes, as SHA-256 has.
inline constexpr std::string_view kFingerprint =
    "sha-256 05:B8:BE:BA:8C:BF:94:93:B2:2B:40:B7:B5:12:F3:18:02:5D:A5:C4:69:AF:F8:52:13:E1:60:"
    "0E:47:CB:7B:78";

// The smallest description the boundary accepts, one bundled audio section. Line numbers:
// v=1, o=2, s=3, t=4, group=5, m=6, c=7, ice-ufrag=8, ice-pwd=9, fingerprint=10, setup=11,
// mid=12, rtpmap=13; an attribute appended to it is on line 14.
inline Lines minimal() {
    return {"v=0",
            "o=- 1 2 IN IP4 127.0.0.1",
            "s=-",
            "t=0 0",
            "a=group:BUNDLE 0",
            "m=audio 9 UDP/TLS/RTP/SAVPF 111 0 96 97",
            "c=IN IP4 0.0.0.0",
            "a=ice-ufrag:abcd",
            "a=ice-pwd:aaaaaaaaaaaaaaaaaaaaaa",
            "a=fingerprint:" + std::string{kFingerprint},
            "a=setup:actpass",
            "a=mid:0",
            "a=rtpmap:111 opus/48000/2"};
}

inline std::string text(const Lines& lines) {
    std::string out;
    for (const std::string& line : lines) {
        out += line;
        out += "\r\n";
    }
    return out;
}

inline Lines with(Lines lines, std::string_view line) {
    lines.emplace_back(line);
    return lines;
}

// Replaces the first line that starts with `prefix`; an empty replacement removes it.
inline Lines replaced(Lines lines, std::string_view prefix, std::string_view by) {
    const auto it =
        std::ranges::find_if(lines, [&](const std::string& l) { return l.starts_with(prefix); });
    if (it == lines.end()) {
        return lines;
    }
    if (by.empty()) {
        lines.erase(it);
    } else {
        *it = by;
    }
    return lines;
}

// Inserts `line` before the one at 1-based `number`.
inline Lines inserted(Lines lines, std::size_t number, std::string_view line) {
    lines.insert(lines.begin() + static_cast<long>(number - 1), std::string{line});
    return lines;
}

// Parse results borrow from the text; the fixture keeps it alive for the test.
class Parsed {
public:
    explicit Parsed(std::string t, const codec::sdp::Limits& limits = {})
        : text_(std::move(t)), result_(codec::sdp::parse(text_, limits)) {}
    explicit Parsed(const Lines& lines) : Parsed(text(lines)) {}
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
    Parsed(Parsed&&) = delete;
    Parsed& operator=(Parsed&&) = delete;
    ~Parsed() = default;

    [[nodiscard]] bool ok() const { return result_.has_value(); }
    [[nodiscard]] const codec::sdp::Session& session() const { return result_.value(); }
    [[nodiscard]] std::optional<codec::sdp::Error> error() const {
        if (ok()) {
            return std::nullopt;
        }
        return result_.error();
    }
    [[nodiscard]] const std::string& source() const { return text_; }

private:
    std::string text_;
    std::expected<codec::sdp::Session, codec::sdp::Error> result_;
};

inline codec::sdp::Error error_at(codec::sdp::ErrorCode code, std::uint32_t line) {
    return codec::sdp::Error{.code = code, .line = line};
}

} // namespace ulw::test
