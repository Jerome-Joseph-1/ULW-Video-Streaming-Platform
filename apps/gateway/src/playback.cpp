#include "playback.hpp"

#include "core/util/hls.hpp"

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

PlaylistFailure from_rewrite(core::hls::PlaylistError e) noexcept {
    switch (e) {
    case core::hls::PlaylistError::Unsigned:
        return PlaylistFailure::Unsigned;
    case core::hls::PlaylistError::Malformed:
    case core::hls::PlaylistError::UnsafeUri:
    case core::hls::PlaylistError::UnroutableVariant:
        return PlaylistFailure::Rejected;
    }
    return PlaylistFailure::Rejected;
}

std::string hls_prefix(const core::VideoId& video) {
    return "videos/" + video.to_string() + "/hls/";
}

std::expected<std::string, PlaylistFailure> fetch(core::ports::IObjectReader& reader,
                                                  const std::string& key_text) {
    const auto key = core::StorageKey::parse(key_text);
    if (!key) {
        return std::unexpected(PlaylistFailure::Broken);
    }
    auto bytes = reader.fetch_small(*key, kMaxStoredPlaylist);
    if (!bytes) {
        return std::unexpected(from_storage(bytes.error()));
    }
    std::string text(bytes->size(), '\0');
    std::ranges::transform(*bytes, text.begin(), [](std::byte b) { return static_cast<char>(b); });
    return text;
}

} // namespace

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
    auto out = core::hls::rewrite_media(
        *media, directory, [&](const core::StorageKey& key) -> std::optional<std::string> {
            auto grant = reader.grant_read(key, request.ttl);
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
        });
    if (!out) {
        return std::unexpected(from_rewrite(out.error()));
    }
    return std::move(*out);
}

} // namespace gateway
