#include "publisher.hpp"

#include "core/models/content_type.hpp"
#include "core/models/storage_key.hpp"
#include "infra/ffmpeg/live_remux.hpp"

#include <fstream>
#include <system_error>
#include <utility>

namespace live {

namespace {

namespace fs = std::filesystem;

// Same types the worker stores its VOD renditions with.
const core::ContentType& segment_type() {
    static const auto type = *core::ContentType::parse("video/iso.segment");
    return type;
}
const core::ContentType& init_type() {
    static const auto type = *core::ContentType::parse("video/mp4");
    return type;
}
const core::ContentType& playlist_type() {
    static const auto type = *core::ContentType::parse("application/vnd.apple.mpegurl");
    return type;
}

constexpr std::string_view kPlaylistName = "index.m3u8";

struct Stored {
    // Nullopt when the stream has no playlist in the store.
    std::optional<MediaPlaylist> playlist;
};

// Every wait the store's own retries allow has been spent by the time an error gets here.
std::expected<Stored, PublishError> read_stored(core::ports::IObjectTransfer& store,
                                                const StreamId& stream, const fs::path& outbox) {
    const auto key = core::StorageKey::parse(stream.key_prefix() + std::string(kPlaylistName));
    if (!key) {
        return std::unexpected(PublishError::StoredPlaylistInvalid);
    }
    const fs::path file = outbox / "stored.m3u8";
    const auto downloaded = store.download(*key, file);
    if (!downloaded) {
        if (downloaded.error() == core::ports::StorageError::NotFound) {
            return Stored{};
        }
        return std::unexpected(PublishError::StoreUnreadable);
    }
    std::ifstream in(file, std::ios::binary);
    // Ours are a few KB; a store answering with more is not our playlist.
    constexpr std::uint64_t kMaxStored = std::uint64_t{1} << 20U;
    if (*downloaded > kMaxStored) {
        return std::unexpected(PublishError::StoredPlaylistInvalid);
    }
    std::string text(*downloaded, '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    auto playlist = parse_media_playlist(text);
    if (!in || !playlist) {
        return std::unexpected(PublishError::StoredPlaylistInvalid);
    }
    return Stored{.playlist = std::move(*playlist)};
}

} // namespace

std::string_view to_string(PublishError e) noexcept {
    switch (e) {
    case PublishError::StoreUnreadable:
        return "store unreadable";
    case PublishError::StoredPlaylistInvalid:
        return "stored playlist invalid";
    case PublishError::AlreadyEnded:
        return "stream already ended";
    case PublishError::UploadFailed:
        return "upload failed";
    case PublishError::PlaylistInconsistent:
        return "ffmpeg playlist inconsistent";
    case PublishError::SegmentsLost:
        return "segments lost";
    }
    return "unknown";
}

Publisher::Publisher(PublisherConfig config, core::ports::IObjectTransfer& store,
                     const core::ports::IClock& clock, LiveWindow window, std::uint32_t epoch,
                     bool resumed)
    : config_(std::move(config)), store_(store), clock_(clock), window_(std::move(window)),
      tracker_(window_.next_sequence()), epoch_(epoch), resumed_(resumed) {}

std::expected<Publisher, PublishError> Publisher::open(PublisherConfig config,
                                                       core::ports::IObjectTransfer& store,
                                                       const core::ports::IClock& clock) {
    std::error_code ec;
    fs::create_directories(config.outbox, ec);
    if (ec) {
        return std::unexpected(PublishError::StoreUnreadable);
    }
    auto stored = read_stored(store, config.stream, config.outbox);
    if (!stored) {
        return std::unexpected(stored.error());
    }
    std::optional<MediaPlaylist> previous = std::move(stored->playlist);
    if (!previous || previous->segments.empty()) {
        LiveWindow window = LiveWindow::fresh(config.window);
        return Publisher(std::move(config), store, clock, std::move(window), 0, false);
    }
    if (previous->ended) {
        return std::unexpected(PublishError::AlreadyEnded);
    }
    // The next run's init segment must not reuse a name the window's segments point at.
    const auto last_epoch = infra::ffmpeg::live_init_epoch(previous->segments.back().init);
    if (!last_epoch) {
        return std::unexpected(PublishError::StoredPlaylistInvalid);
    }
    LiveWindow window = LiveWindow::resume(config.window, std::move(*previous));
    return Publisher(std::move(config), store, clock, std::move(window), *last_epoch + 1, true);
}

void Publisher::begin_epoch() {
    window_.begin_epoch(clock_.wall_now());
}

std::expected<void, PublishError> Publisher::put(const fs::path& file, std::string_view name,
                                                 const core::ContentType& type) {
    const auto key = core::StorageKey::parse(config_.stream.key_prefix() + std::string(name));
    if (!key) {
        return std::unexpected(PublishError::PlaylistInconsistent);
    }
    if (!store_.upload(file, *key, type)) {
        return std::unexpected(PublishError::UploadFailed);
    }
    return {};
}

std::expected<void, PublishError> Publisher::publish_playlist(const MediaPlaylist& playlist) {
    const fs::path file = config_.outbox / kPlaylistName;
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << render_media_playlist(playlist);
        if (!out) {
            return std::unexpected(PublishError::UploadFailed);
        }
    }
    return put(file, kPlaylistName, playlist_type());
}

std::expected<std::size_t, PublishError> Publisher::pump(std::string_view ffmpeg_playlist) {
    const auto ready = tracker_.scan(ffmpeg_playlist, [this](std::string_view name) {
        std::error_code ec;
        const auto size = fs::file_size(config_.media_dir / name, ec);
        return ec ? std::nullopt : std::optional<std::uint64_t>(size);
    });
    if (!ready) {
        // Caught between ffmpeg's write and rename, or not written yet: the next look sees it.
        if (ready.error() == ScanError::Unreadable) {
            return 0;
        }
        return std::unexpected(ready.error() == ScanError::Lost
                                   ? PublishError::SegmentsLost
                                   : PublishError::PlaylistInconsistent);
    }
    std::size_t published = 0;
    std::optional<PublishError> failure;
    for (const CompletedSegment& segment : *ready) {
        if (segment.init != uploaded_init_) {
            if (const auto sent = put(config_.media_dir / segment.init, segment.init, init_type());
                !sent) {
                failure = sent.error();
                break;
            }
            uploaded_init_ = segment.init;
        }
        const fs::path file = config_.media_dir / segment.uri;
        if (const auto sent = put(file, segment.uri, segment_type()); !sent) {
            failure = sent.error();
            break;
        }
        if (window_.exceeds_target(segment.duration)) {
            ++overlong_;
        }
        window_.add(segment.uri, segment.duration, segment.init);
        tracker_.handled(segment.sequence);
        dirty_ = true;
        ++published;
        std::error_code ec;
        fs::remove(file, ec);
    }
    if (dirty_) {
        if (const auto sent = publish_playlist(window_.playlist()); sent) {
            dirty_ = false;
        } else if (!failure) {
            failure = sent.error();
        }
    }
    if (failure) {
        return std::unexpected(*failure);
    }
    return published;
}

std::expected<void, PublishError> Publisher::finish(std::string_view ffmpeg_playlist) {
    if (const auto pumped = pump(ffmpeg_playlist); !pumped) {
        return std::unexpected(pumped.error());
    }
    if (window_.playlist().segments.empty()) {
        return {};
    }
    return publish_playlist(window_.ended());
}

} // namespace live
