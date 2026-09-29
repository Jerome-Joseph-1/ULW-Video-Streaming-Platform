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
        out.append(rest.substr(0, eq + 1));
        rest.remove_prefix(eq + 1);
        std::string_view value;
        if (rest.starts_with('"')) {
            const std::size_t close = rest.find('"', 1);
            if (close == std::string_view::npos) {
                return std::unexpected(PlaylistError::Malformed);
            }
            value = rest.substr(0, close + 1);
        } else {
            if (name == "URI") {
                return std::unexpected(PlaylistError::Malformed);
            }
            value = rest.substr(0, rest.find(','));
        }
        rest.remove_prefix(value.size());
        if (!rest.empty() && !rest.starts_with(',')) {
            return std::unexpected(PlaylistError::Malformed);
        }
        if (name == "URI") {
            auto mapped = map(value.substr(1, value.size() - 2));
            if (!mapped) {
                return std::unexpected(mapped.error());
            }
            out.append(1, '"').append(*mapped).append(1, '"');
        } else {
            out.append(value);
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
std::expected<std::string, PlaylistError> rewrite(std::string_view text, const Map& map) {
    if (auto r = check_header(text); !r) {
        return std::unexpected(r.error());
    }
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
        } else {
            out.append(line);
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
    }
    return "playlist error";
}

std::expected<std::vector<std::string_view>, PlaylistError>
list_renditions(std::string_view master) {
    if (auto r = check_header(master); !r) {
        return std::unexpected(r.error());
    }
    std::vector<std::string_view> names;
    auto r =
        for_each_line(master, [&](std::string_view line) -> std::expected<void, PlaylistError> {
            if (line.empty() || line.starts_with('#')) {
                return {};
            }
            auto name = rendition_of(line);
            if (!name) {
                return std::unexpected(name.error());
            }
            if (std::ranges::find(names, *name) == names.end()) {
                names.push_back(*name);
            }
            return {};
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
