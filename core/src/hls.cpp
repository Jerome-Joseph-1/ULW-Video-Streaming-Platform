#include "core/util/hls.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>

namespace core::hls {

namespace {

constexpr std::string_view kHeader = "#EXTM3U";
constexpr std::string_view kMediaPlaylist = "/index.m3u8";

// RFC 8216 tags whose attribute lists may carry a URI. Only these are scanned: in any other
// tag a quoted value could hold text that merely looks like a URI attribute.
constexpr std::array<std::string_view, 9> kUriTags{
    "#EXT-X-MAP:",
    "#EXT-X-KEY:",
    "#EXT-X-SESSION-KEY:",
    "#EXT-X-MEDIA:",
    "#EXT-X-I-FRAME-STREAM-INF:",
    "#EXT-X-SESSION-DATA:",
    "#EXT-X-PART:",
    "#EXT-X-PRELOAD-HINT:",
    "#EXT-X-RENDITION-REPORT:",
};

// Tags that never carry a URI, passed through as they are. Anything else that starts with
// #EXT is refused: a tag this list does not know may name a URL a player acts on
// (EXT-X-CONTENT-STEERING's SERVER-URI, an interstitial EXT-X-DATERANGE's X-ASSET-URI), which
// would reach the viewer unsigned and unchecked.
constexpr std::array<std::string_view, 19> kUriFreeTags{
    "#EXTM3U",
    "#EXT-X-VERSION",
    "#EXT-X-INDEPENDENT-SEGMENTS",
    "#EXT-X-START",
    "#EXT-X-STREAM-INF",
    "#EXT-X-TARGETDURATION",
    "#EXT-X-MEDIA-SEQUENCE",
    "#EXT-X-DISCONTINUITY-SEQUENCE",
    "#EXT-X-PLAYLIST-TYPE",
    "#EXT-X-I-FRAMES-ONLY",
    "#EXT-X-ENDLIST",
    "#EXTINF",
    "#EXT-X-BYTERANGE",
    "#EXT-X-DISCONTINUITY",
    "#EXT-X-PROGRAM-DATE-TIME",
    "#EXT-X-GAP",
    "#EXT-X-BITRATE",
    "#EXT-X-SERVER-CONTROL",
    "#EXT-X-PART-INF",
};

// Of those, the ones with attribute lists. Their attributes are checked too, so that a custom
// X-...-URI attribute cannot ride along on a known tag.
constexpr std::array<std::string_view, 4> kAttributeTags{
    "#EXT-X-START:",
    "#EXT-X-STREAM-INF:",
    "#EXT-X-SERVER-CONTROL:",
    "#EXT-X-PART-INF:",
};

bool is_tag(std::string_view line, std::string_view tag) noexcept {
    return line.starts_with(tag) && (line.size() == tag.size() || line[tag.size()] == ':');
}

bool is_uri_free(std::string_view line) noexcept {
    return std::ranges::any_of(kUriFreeTags,
                               [&](std::string_view tag) { return is_tag(line, tag); });
}

bool has_attribute_list(std::string_view line) noexcept {
    return std::ranges::any_of(kAttributeTags,
                               [&](std::string_view tag) { return line.starts_with(tag); });
}

// Calls `fn` with each line, without its terminator; a CR before the LF is dropped too.
template <class Fn> std::expected<void, PlaylistError> for_each_line(std::string_view text, Fn fn) {
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        // A stray CR, NUL or other control byte is a line break to some parsers and not to
        // others; nothing the worker writes has one.
        if (std::ranges::any_of(line,
                                [](char c) { return static_cast<unsigned char>(c) < 0x20; })) {
            return std::unexpected(PlaylistError::Malformed);
        }
        if (auto r = fn(line); !r) {
            return r;
        }
    }
    return {};
}

std::expected<void, PlaylistError> check_header(std::string_view text) {
    const std::string_view first = text.substr(0, text.find('\n'));
    if (first != kHeader && first != "#EXTM3U\r") {
        return std::unexpected(PlaylistError::Malformed);
    }
    return {};
}

bool has_uri_attributes(std::string_view line) noexcept {
    return std::ranges::any_of(kUriTags,
                               [&](std::string_view tag) { return line.starts_with(tag); });
}

// The key a relative URI names from `directory`. Everything StorageKey refuses is refused
// here: "..", ".", empty segments (so a leading '/'), and any character a scheme, query,
// fragment or percent-escape needs.
std::expected<StorageKey, PlaylistError> resolve(std::string_view directory, std::string_view uri) {
    if (uri.empty()) {
        return std::unexpected(PlaylistError::UnsafeUri);
    }
    std::string text;
    text.reserve(directory.size() + uri.size());
    text.append(directory).append(uri);
    auto key = StorageKey::parse(text);
    if (!key) {
        return std::unexpected(PlaylistError::UnsafeUri);
    }
    return std::move(*key);
}

// The rendition a master playlist URI names: exactly one segment, then /index.m3u8.
std::expected<std::string_view, PlaylistError> rendition_of(std::string_view uri) {
    if (!resolve({}, uri)) {
        return std::unexpected(PlaylistError::UnsafeUri);
    }
    if (!uri.ends_with(kMediaPlaylist)) {
        return std::unexpected(PlaylistError::UnroutableVariant);
    }
    const std::string_view name = uri.substr(0, uri.size() - kMediaPlaylist.size());
    if (name.find('/') != std::string_view::npos) {
        return std::unexpected(PlaylistError::UnroutableVariant);
    }
    return name;
}

// The value that starts `rest`, quotes included, for the attribute `name`. A URI must be a
// closed quoted string; any other value may run to the next comma.
std::expected<std::string_view, PlaylistError> attribute_value(std::string_view rest,
                                                               std::string_view name) {
    if (!rest.starts_with('"')) {
        if (name == "URI") {
            return std::unexpected(PlaylistError::Malformed);
        }
        return rest.substr(0, rest.find(','));
    }
    const std::size_t close = rest.find('"', 1);
    if (close == std::string_view::npos) {
        return std::unexpected(PlaylistError::Malformed);
    }
    return rest.substr(0, close + 1);
}

// Copies a tag line to `out` with each quoted URI attribute replaced by map(uri).
template <class Map>
std::expected<void, PlaylistError> rewrite_attributes(std::string_view line, std::string& out,
                                                      const Map& map) {
    const std::size_t colon = line.find(':');
    out.append(line.substr(0, colon + 1));
    std::string_view rest = line.substr(colon + 1);
    while (!rest.empty()) {
        const std::size_t eq = rest.find('=');
        if (eq == std::string_view::npos) {
            return std::unexpected(PlaylistError::Malformed);
        }
        const std::string_view name = rest.substr(0, eq);
        // A URI under any other name is one this rewriter does not handle.
        if (name != "URI" && name.ends_with("URI")) {
            return std::unexpected(PlaylistError::UnknownTag);
        }
        out.append(rest.substr(0, eq + 1));
        rest.remove_prefix(eq + 1);
        const auto value = attribute_value(rest, name);
        if (!value) {
            return std::unexpected(value.error());
        }
        rest.remove_prefix(value->size());
        if (!rest.empty() && !rest.starts_with(',')) {
            return std::unexpected(PlaylistError::Malformed);
        }
        if (name == "URI") {
            auto mapped = map(value->substr(1, value->size() - 2));
            if (!mapped) {
                return std::unexpected(mapped.error());
            }
            out.append(1, '"').append(*mapped).append(1, '"');
        } else {
            out.append(*value);
        }
        if (!rest.empty()) {
            out.append(1, ',');
            rest.remove_prefix(1);
        }
    }
    return {};
}

// Blank lines are dropped: RFC 8216 ignores them, and some players mistake one after #EXTINF
// for a segment with an empty URI.
template <class Map>
std::expected<std::string, PlaylistError> rewrite(std::string_view text, const Map& map_one) {
    if (auto r = check_header(text); !r) {
        return std::unexpected(r.error());
    }
    std::size_t uris = 0;
    const auto map = [&](std::string_view uri) -> std::expected<std::string, PlaylistError> {
        if (++uris > kMaxPlaylistUris) {
            return std::unexpected(PlaylistError::TooManyUris);
        }
        return map_one(uri);
    };
    const auto no_uri = [](std::string_view) -> std::expected<std::string, PlaylistError> {
        return std::unexpected(PlaylistError::UnknownTag);
    };
    std::string out;
    // Signed URLs make a media playlist several times longer than the stored one.
    out.reserve(text.size() * 4);
    auto r = for_each_line(text, [&](std::string_view line) -> std::expected<void, PlaylistError> {
        if (line.empty()) {
            return {};
        }
        if (!line.starts_with('#')) {
            auto mapped = map(line);
            if (!mapped) {
                return std::unexpected(mapped.error());
            }
            out.append(*mapped);
        } else if (has_uri_attributes(line)) {
            if (auto a = rewrite_attributes(line, out, map); !a) {
                return a;
            }
        } else if (has_attribute_list(line)) {
            if (auto a = rewrite_attributes(line, out, no_uri); !a) {
                return a;
            }
        } else if (is_uri_free(line) || !line.starts_with("#EXT")) {
            // A tag without attributes, or a comment, which players ignore.
            out.append(line);
        } else {
            return std::unexpected(PlaylistError::UnknownTag);
        }
        out.append(1, '\n');
        return {};
    });
    if (!r) {
        return std::unexpected(r.error());
    }
    return out;
}

} // namespace

std::string_view to_string(PlaylistError e) noexcept {
    switch (e) {
    case PlaylistError::Malformed:
        return "malformed playlist";
    case PlaylistError::UnsafeUri:
        return "unsafe playlist uri";
    case PlaylistError::UnroutableVariant:
        return "unroutable variant uri";
    case PlaylistError::Unsigned:
        return "playlist uri not signed";
    case PlaylistError::UnknownTag:
        return "playlist tag or attribute that may carry a uri";
    case PlaylistError::TooManyUris:
        return "too many playlist uris";
    }
    return "playlist error";
}

// The same walk as rewrite_master, so every URI it would route, tag attributes included, is
// a rendition the gateway will serve.
std::expected<std::vector<std::string_view>, PlaylistError>
list_renditions(std::string_view master) {
    std::vector<std::string_view> names;
    auto r =
        rewrite(master, [&](std::string_view uri) -> std::expected<std::string, PlaylistError> {
            auto name = rendition_of(uri);
            if (!name) {
                return std::unexpected(name.error());
            }
            if (std::ranges::find(names, *name) == names.end()) {
                names.push_back(*name);
            }
            return std::string{};
        });
    if (!r) {
        return std::unexpected(r.error());
    }
    return names;
}

std::expected<std::string, PlaylistError> rewrite_master(std::string_view master,
                                                         std::string_view route_prefix) {
    return rewrite(master, [&](std::string_view uri) -> std::expected<std::string, PlaylistError> {
        auto name = rendition_of(uri);
        if (!name) {
            return std::unexpected(name.error());
        }
        std::string routed;
        routed.reserve(route_prefix.size() + uri.size());
        routed.append(route_prefix).append(*name).append(kMediaPlaylist);
        return routed;
    });
}

std::expected<std::string, PlaylistError>
rewrite_media(std::string_view media, std::string_view directory, const Signer& sign) {
    return rewrite(media, [&](std::string_view uri) -> std::expected<std::string, PlaylistError> {
        auto key = resolve(directory, uri);
        if (!key) {
            return std::unexpected(key.error());
        }
        auto url = sign(*key);
        if (!url) {
            return std::unexpected(PlaylistError::Unsigned);
        }
        return std::move(*url);
    });
}

Seconds presign_ttl(Millis duration) noexcept {
    const auto twice = std::chrono::ceil<Seconds>(duration * 2);
    return std::max<Seconds>(twice, std::chrono::hours(1));
}

} // namespace core::hls
