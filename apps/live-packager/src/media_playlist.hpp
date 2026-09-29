#pragma once

#include "core/util/time.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace live {

using Micros = std::chrono::microseconds;

struct Segment {
    std::string uri;
    Micros duration{};
    // The init segment this one is decoded with: the EXT-X-MAP in force at it.
    std::string init;
    // The stream's timeline breaks before this segment (EXT-X-DISCONTINUITY).
    bool discontinuity = false;
    // Millisecond precision, which is all the playlist text carries.
    std::optional<core::WallTime> program_date_time;
};

struct MediaPlaylist {
    std::uint32_t target_seconds = 0;
    std::uint64_t media_sequence = 0;
    std::uint64_t discontinuity_sequence = 0;
    bool ended = false;
    std::vector<Segment> segments;
};

enum class PlaylistError : std::uint8_t {
    // No #EXTM3U first line.
    NotAPlaylist,
    // A tag whose value does not parse, or a URI line no EXTINF announced.
    Malformed,
    // More segments than any window this packager writes.
    TooLong,
};

// The longest window is 64 segments and ffmpeg lists twice that (config.cpp); a playlist of
// more is not one of ours, and this bounds the work of reading it.
inline constexpr std::size_t kMaxSegments = 256;

[[nodiscard]] std::expected<MediaPlaylist, PlaylistError>
parse_media_playlist(std::string_view text);

// The tags players need for a live window: version 7 (fMP4 needs 6, independent segments 6),
// and an EXT-X-MAP wherever the init segment changes.
[[nodiscard]] std::string render_media_playlist(const MediaPlaylist& playlist);

} // namespace live
