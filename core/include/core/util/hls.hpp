#pragma once

#include "core/models/storage_key.hpp"
#include "core/util/time.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Rewrites the HLS playlists the worker stores into the ones a viewer is served (ADR-0024).
// Pure text in, text out: fetching the stored playlist and signing URLs are the caller's.
namespace core::hls {

enum class PlaylistError : std::uint8_t {
    // No #EXTM3U header, a control character inside a line, or a tag whose URI attribute is
    // not a closed quoted string.
    Malformed,
    // A URI that climbs out of its directory, is absolute, names a scheme or a query, or
    // does not form a storage key.
    UnsafeUri,
    // A master playlist URI that is not <rendition>/index.m3u8, which no gateway route serves.
    UnroutableVariant,
    // The signer refused a URI.
    Unsigned,
    // A tag the rewriter does not know, or an attribute other than URI whose name ends in URI:
    // either could hand a player a URL no one checked.
    UnknownTag,
    // More URIs than kMaxPlaylistUris.
    TooManyUris,
};

// The worker's longest video is 12 h in 4 s segments: 10,800 segment URIs and one init
// segment. Rounded up to 2^14 for slack. It bounds the signing work and the response too:
// at ~600 bytes a signed URL, 16,384 of them come to 9.8 MB.
inline constexpr std::size_t kMaxPlaylistUris = 16'384;

[[nodiscard]] std::string_view to_string(PlaylistError e) noexcept;

// Returns the presigned URL for an object, or nullopt when it cannot grant one.
using Signer = std::function<std::optional<std::string>(const StorageKey& key)>;

// The renditions a master playlist lists, in its order: those of its variant URIs and of the
// URI attributes of its tags (EXT-X-MEDIA, EXT-X-I-FRAME-STREAM-INF), exactly what
// rewrite_master routes. Names are single key segments.
[[nodiscard]] std::expected<std::vector<std::string_view>, PlaylistError>
list_renditions(std::string_view master);

// Every variant URI, resolved against the master's directory, becomes
// `<route_prefix><rendition>/index.m3u8`.
[[nodiscard]] std::expected<std::string, PlaylistError>
rewrite_master(std::string_view master, std::string_view route_prefix);

// Every segment URI and every URI attribute (EXT-X-MAP's among them), resolved against
// `directory`, becomes the URL `sign` grants for that key. `directory` is a key prefix that
// ends in '/'.
[[nodiscard]] std::expected<std::string, PlaylistError>
rewrite_media(std::string_view media, std::string_view directory, const Signer& sign);

// How long a segment URL stays valid: twice the video's length, so pauses and seeks fit in,
// and never under an hour, so a short video is not cut off mid-view. The store caps it at the
// longest presign its backend allows.
[[nodiscard]] Seconds presign_ttl(Millis duration) noexcept;

} // namespace core::hls
