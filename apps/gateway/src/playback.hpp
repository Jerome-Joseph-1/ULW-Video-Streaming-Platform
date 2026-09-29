#pragma once

#include "core/models/ids.hpp"
#include "core/ports/storage.hpp"
#include "core/util/time.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace gateway {

// The worker's stored playlists run one line pair per 4 s segment: "#EXTINF:4.000000,\n" and
// "seg_00000.m4s\n", 32 bytes. The longest video it takes, 12 h, has 10,800 segments, so
// 346 KB; anything past 512 KiB is not a playlist the worker wrote.
inline constexpr std::size_t kMaxStoredPlaylist = std::size_t{512} * 1024;

enum class PlaylistKind : std::uint8_t { Master, Media };

enum class PlaylistFailure : std::uint8_t {
    // The master does not list the rendition asked for.
    NoSuchRendition,
    // The store is throttling or unreachable; the viewer may retry.
    Unavailable,
    // The stored playlist breaks a rewriting rule: the worker wrote something wrong.
    Rejected,
    // The store would not grant a URL for a segment.
    Unsigned,
    // A ready video whose playlists are missing, oversized or unreadable.
    Broken,
    // Live only: no stream by that id has published a playlist.
    Absent,
};

struct PlaylistRequest {
    PlaylistKind kind = PlaylistKind::Master;
    core::VideoId video;
    // Media playlists only.
    std::string rendition;
    core::Seconds ttl{};
};

// Fetches the stored playlist, and for a media playlist the master it must be listed in, and
// rewrites it for the viewer. Blocks on the object store: offload pool only.
//
// `local_read_url` is where a development server publishes the filesystem backend's objects,
// or "" for none. That backend's read grants are file paths, which no browser can fetch.
[[nodiscard]] std::expected<std::string, PlaylistFailure>
build_playlist(core::ports::IObjectReader& reader, const PlaylistRequest& request,
               std::string_view local_read_url);

// The packager's window is at most 64 segments (apps/live-packager/src/config.cpp), each at
// most about 180 bytes of tags and name: a program date time (50), an EXTINF (18), a name
// with a 20-digit epoch and index (50), and a discontinuity with a new EXT-X-MAP (60) after a
// restart. 64 x 180 = 11.5 KB; anything past 64 KiB is not a playlist the packager wrote.
inline constexpr std::size_t kMaxStoredLivePlaylist = std::size_t{64} * 1024;

// The packager's segment length is 2 to 10 s (ADR-0046); a target duration outside what any
// packager writes is refused rather than cached for an unforeseen time.
inline constexpr std::uint64_t kMinLiveTargetSeconds = 2;
inline constexpr std::uint64_t kMaxLiveTargetSeconds = 10;

// An ended playlist never changes again (ADR-0047 refuses to restart an ended stream), so it
// is held as long as a VOD playlist is (ADR-0024).
inline constexpr core::Millis kEndedLiveFreshFor{60'000};

// Every URL in a cached live playlist must outlive the copy. The copy is held for at most
// kEndedLiveFreshFor, and a viewer uses a live playlist's URLs within a window of segments;
// VOD's one-hour floor (ADR-0024) covers both with room for a paused viewer to resume.
inline constexpr core::Seconds kLivePresignTtl{3600};
static_assert(kEndedLiveFreshFor < kLivePresignTtl);

// Letters, digits, '_' and '-', 1 to 64 of them: the ids live_packager accepts
// (apps/live-packager/src/stream_id.cpp), and so every prefix it can have written.
[[nodiscard]] bool valid_stream_id(std::string_view id) noexcept;

struct LivePlaylist {
    std::string body;
    // How long the rewritten copy may be served before the store is asked again.
    core::Millis fresh_for{};
    // It carries EXT-X-ENDLIST.
    bool ended = false;
};

// Fetches `live/<stream>/index.m3u8` and signs every URI in it for kLivePresignTtl. Blocks on
// the object store: offload pool only.
[[nodiscard]] std::expected<LivePlaylist, PlaylistFailure>
build_live_playlist(core::ports::IObjectReader& reader, std::string_view stream,
                    std::string_view local_read_url);

} // namespace gateway
