#include "playback.hpp"

#include "core/util/hls.hpp"
#include "core/util/parse.hpp"

#include <algorithm>
#include <optional>
#include <vector>

namespace gateway {

namespace {

using core::ports::StorageError;

PlaylistFailure from_storage(StorageError e) noexcept {
    switch (e) {
    case StorageError::Throttled:
    case StorageError::Transient:
        return PlaylistFailure::Unavailable;
    // A ready video has every playlist its master names; one missing is not the viewer's
    // doing, and neither is one over the size bound (Permanent).
    case StorageError::NotFound:
    case StorageError::AlreadyExists:
    case StorageError::PreconditionFailed:
    case StorageError::Unauthorized:
    case StorageError::Permanent:
    case StorageError::Corrupt:
        return PlaylistFailure::Broken;
    }
    return PlaylistFailure::Broken;
}

// A live playlist that is not there yet is the ordinary state of a stream before its first
// segment, not a fault.
PlaylistFailure from_live_storage(StorageError e) noexcept {
    switch (e) {
    case StorageError::NotFound:
        return PlaylistFailure::Absent;
    case StorageError::Throttled:
    case StorageError::Transient:
    case StorageError::AlreadyExists:
    case StorageError::PreconditionFailed:
    case StorageError::Unauthorized:
    case StorageError::Permanent:
    case StorageError::Corrupt:
        return from_storage(e);
    }
    return PlaylistFailure::Broken;
}

PlaylistFailure from_rewrite(core::hls::PlaylistError e) noexcept {
    switch (e) {
    case core::hls::PlaylistError::Unsigned:
        return PlaylistFailure::Unsigned;
    case core::hls::PlaylistError::Malformed:
    case core::hls::PlaylistError::UnsafeUri:
    case core::hls::PlaylistError::UnroutableVariant:
    case core::hls::PlaylistError::UnknownTag:
    case core::hls::PlaylistError::TooManyUris:
        return PlaylistFailure::Rejected;
    }
    return PlaylistFailure::Rejected;
}

std::string hls_prefix(const core::VideoId& video) {
    return "videos/" + video.to_string() + "/hls/";
}

std::expected<std::string, PlaylistFailure>
fetch(core::ports::IObjectReader& reader, const std::string& key_text,
      std::size_t max = kMaxStoredPlaylist,
      PlaylistFailure (*classify)(StorageError) noexcept = from_storage) {
    const auto key = core::StorageKey::parse(key_text);
    if (!key) {
        return std::unexpected(PlaylistFailure::Broken);
    }
    auto bytes = reader.fetch_small(*key, max);
    if (!bytes) {
        return std::unexpected(classify(bytes.error()));
    }
    std::string text(bytes->size(), '\0');
    std::ranges::transform(*bytes, text.begin(), [](std::byte b) { return static_cast<char>(b); });
    return text;
}

core::hls::Signer signer(core::ports::IObjectReader& reader, core::Seconds ttl,
                         std::string_view local_read_url) {
    return
        [&reader, ttl, local_read_url](const core::StorageKey& key) -> std::optional<std::string> {
            auto grant = reader.grant_read(key, ttl);
            if (!grant) {
                return std::nullopt;
            }
            switch (grant->kind) {
            case core::ports::ReadGrant::Kind::RedirectUrl:
                return std::move(grant->value);
            case core::ports::ReadGrant::Kind::ServeLocally:
                if (local_read_url.empty()) {
                    return std::nullopt;
                }
                return std::string(local_read_url) + "/" + key.str();
            }
            return std::nullopt;
        };
}

struct LiveShape {
    std::uint64_t target_seconds = 0;
    bool ended = false;
};

// Only what the cache needs. It runs before rewrite_media, which then vets every line and
// refuses the playlist over any it does not accept, so nothing here is served unchecked.
std::optional<LiveShape> live_shape(std::string_view text) {
    constexpr std::string_view kTarget = "#EXT-X-TARGETDURATION:";
    LiveShape shape;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        if (line.starts_with(kTarget)) {
            const auto t = core::parse_integer<std::uint64_t>(line.substr(kTarget.size()));
            if (!t) {
                return std::nullopt;
            }
            shape.target_seconds = *t;
        } else if (line == "#EXT-X-ENDLIST") {
            shape.ended = true;
        }
    }
    if (shape.target_seconds < kMinLiveTargetSeconds ||
        shape.target_seconds > kMaxLiveTargetSeconds) {
        return std::nullopt;
    }
    return shape;
}

} // namespace

bool valid_stream_id(std::string_view id) noexcept {
    constexpr std::size_t kMaxLength = 64;
    const auto allowed = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    };
    return !id.empty() && id.size() <= kMaxLength && std::ranges::all_of(id, allowed);
}

std::expected<LivePlaylist, PlaylistFailure> build_live_playlist(core::ports::IObjectReader& reader,
                                                                 std::string_view stream,
                                                                 std::string_view local_read_url) {
    if (!valid_stream_id(stream)) {
        return std::unexpected(PlaylistFailure::Absent);
    }
    const std::string directory = "live/" + std::string(stream) + "/";
    auto stored =
        fetch(reader, directory + "index.m3u8", kMaxStoredLivePlaylist, from_live_storage);
    if (!stored) {
        return std::unexpected(stored.error());
    }
    const auto shape = live_shape(*stored);
    if (!shape) {
        return std::unexpected(PlaylistFailure::Rejected);
    }
    auto out = core::hls::rewrite_media(*stored, directory,
                                        signer(reader, kLivePresignTtl, local_read_url));
    if (!out) {
        return std::unexpected(from_rewrite(out.error()));
    }
    // The packager replaces the playlist once per target duration T, and a player reloads it
    // about as often (RFC 8216 6.3.4). Held for T/2, the copy a viewer gets is never more than
    // half a segment behind the store's, so a reload one T after the last always finds the
    // newer playlist, while every viewer of the stream together costs the store two reads per
    // segment.
    const core::Millis fresh_for =
        shape->ended
            ? kEndedLiveFreshFor
            : core::Millis{static_cast<core::Millis::rep>(shape->target_seconds * 1000 / 2)};
    return LivePlaylist{.body = std::move(*out), .fresh_for = fresh_for, .ended = shape->ended};
}

std::expected<std::string, PlaylistFailure> build_playlist(core::ports::IObjectReader& reader,
                                                           const PlaylistRequest& request,
                                                           std::string_view local_read_url) {
    const std::string prefix = hls_prefix(request.video);
    auto master = fetch(reader, prefix + "master.m3u8");
    if (!master) {
        return master;
    }
    if (request.kind == PlaylistKind::Master) {
        auto out =
            core::hls::rewrite_master(*master, "/api/v1/videos/" + request.video.to_string() + "/");
        if (!out) {
            return std::unexpected(from_rewrite(out.error()));
        }
        return std::move(*out);
    }

    // Only a name the master lists is ever turned into a key, so a request cannot make the
    // gateway read, or sign anything under, a directory the worker did not publish.
    const auto names = core::hls::list_renditions(*master);
    if (!names) {
        return std::unexpected(from_rewrite(names.error()));
    }
    if (std::ranges::find(*names, request.rendition) == names->end()) {
        return std::unexpected(PlaylistFailure::NoSuchRendition);
    }
    const std::string directory = prefix + request.rendition + "/";
    auto media = fetch(reader, directory + "index.m3u8");
    if (!media) {
        return media;
    }
    auto out =
        core::hls::rewrite_media(*media, directory, signer(reader, request.ttl, local_read_url));
    if (!out) {
        return std::unexpected(from_rewrite(out.error()));
    }
    return std::move(*out);
}

} // namespace gateway
