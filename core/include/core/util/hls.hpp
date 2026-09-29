#pragma once

#include "core/models/storage_key.hpp"
#include "core/util/time.hpp"

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
};

[[nodiscard]] std::string_view to_string(PlaylistError e) noexcept;

// Returns the presigned URL for an object, or nullopt when it cannot grant one.
using Signer = std::function<std::optional<std::string>(const StorageKey& key)>;

// The renditions a master playlist lists, in its order. Names are single key segments.
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
