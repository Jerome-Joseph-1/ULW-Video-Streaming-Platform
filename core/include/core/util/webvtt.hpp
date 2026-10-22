#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

// Checks a WebVTT file an owner uploads as a video's captions (ADR-0139) before it is stored
// and handed to players. Pure text in: storing it and serving it are the caller's.
namespace core::webvtt {

// A 2 h film's subtitles run about 1,500 cues of 60 to 100 bytes, 150 KB; 512 KiB holds six
// hours of dialogue as dense. The body is held whole while it is checked, so the bound is also
// what one upload of captions costs the gateway in memory.
inline constexpr std::size_t kMaxFileBytes = std::size_t{512} * 1024;

enum class Problem : std::uint8_t {
    // Not UTF-8 (a stray, overlong or surrogate sequence, or past U+10FFFF), or a NUL.
    Encoding,
    // The first line is not "WEBVTT", alone or followed by a space or a tab and anything, after
    // an optional byte order mark; or a header line holds "-->".
    Header,
    // A cue's timing line is not "<start> --> <end>" with each time [hh:]mm:ss.ttt (minutes
    // and seconds below 60), or its end is not after its start.
    Timing,
    // A cue starts before the cue ahead of it.
    Order,
    // A block that is neither a cue nor NOTE, nor STYLE or REGION ahead of every cue; or "-->"
    // inside a note, a style, a region, an identifier or a cue's text.
    Block,
};

[[nodiscard]] std::string_view to_string(Problem p) noexcept;

struct Error {
    Problem problem = Problem::Block;
    // 1-based: the line the problem was found on.
    std::size_t line = 0;
};

// Whether `text` is a WebVTT file every player parses as its author meant: the rules of the
// WebVTT file format that a parser would otherwise recover from in its own way. Lines may end
// in CR LF, LF or CR. Cue settings and the text of cues are not looked into beyond "-->".
[[nodiscard]] std::expected<void, Error> check(std::string_view text);

} // namespace core::webvtt
