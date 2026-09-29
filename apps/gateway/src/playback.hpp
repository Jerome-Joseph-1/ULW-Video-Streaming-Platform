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

} // namespace gateway
