#include "publisher.hpp"

#include "core/models/content_type.hpp"
#include "core/models/storage_key.hpp"
#include "infra/ffmpeg/live_remux.hpp"

#include <algorithm>
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
const core::ContentType& claim_type() {
    static const auto type = *core::ContentType::parse("text/plain");
    return type;
}
const core::ContentType& playlist_type() {
    static const auto type = *core::ContentType::parse("application/vnd.apple.mpegurl");
    return type;
}

constexpr std::string_view kPlaylistName = "index.m3u8";
// More claims than restarts a stream has ever had: past it something else is wrong.
constexpr int kMaxClaimAttempts = 16;
// Epochs are 32-bit in the init segment's name; well short of that is far past any stream.
constexpr std::int64_t kMaxEpoch = std::int64_t{1} << 30U;

std::string epoch_claim_name(std::uint32_t epoch) {
    return "epoch_" + std::to_string(epoch);
}

struct Stored {
    // Nullopt when the stream has no playlist in the store.
    std::optional<MediaPlaylist> playlist;
};

// The epoch the newest init segment in the playlist carries; -1 for an empty or absent one.
std::expected<std::int64_t, PublishError> last_epoch(const std::optional<MediaPlaylist>& playlist) {
    if (!playlist || playlist->segments.empty()) {
        return -1;
    }
    const auto epoch = infra::ffmpeg::live_init_epoch(playlist->segments.back().init);
    if (!epoch) {
        return std::unexpected(PublishError::StoredPlaylistInvalid);
    }
    return *epoch;
}

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

// Refuses a stored playlist a run must not continue: an ended stream, or one cut for another
// segment length.
std::optional<PublishError> check_resumable(const std::optional<MediaPlaylist>& playlist,
                                            const WindowConfig& window) {
    if (!playlist) {
        return std::nullopt;
    }
    if (playlist->ended) {
        return PublishError::AlreadyEnded;
    }
    if (!playlist->segments.empty() && playlist->target_seconds != window.target_seconds) {
        return PublishError::SegmentLengthChanged;
    }
    return std::nullopt;
}

// Claims the first free epoch at or above `first` by create-only put. Claims a dead run left
// behind, one that never published, are stepped over: the put refuses them.
std::expected<std::uint32_t, PublishError>
claim_from(core::ports::IObjectTransfer& store, const PublisherConfig& config, std::int64_t first) {
    const fs::path file = config.outbox / "claim";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "claimed\n";
        if (!out) {
            return std::unexpected(PublishError::ClaimFailed);
        }
    }
    for (std::int64_t epoch = first; epoch < kMaxEpoch; ++epoch) {
        const auto name = epoch_claim_name(static_cast<std::uint32_t>(epoch));
        const auto key = core::StorageKey::parse(config.stream.key_prefix() + name);
        if (!key) {
            return std::unexpected(PublishError::ClaimFailed);
        }
        const auto put = store.upload_new(file, *key, claim_type());
        if (put) {
            return static_cast<std::uint32_t>(epoch);
        }
        if (put.error() != core::ports::StorageError::AlreadyExists) {
            return std::unexpected(PublishError::ClaimFailed);
        }
    }
    return std::unexpected(PublishError::ClaimFailed);
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
    case PublishError::SegmentLengthChanged:
        return "stored playlist was cut for another segment length";
    case PublishError::ClaimFailed:
        return "epoch claim failed";
    case PublishError::Superseded:
        return "superseded by a newer packager of this stream";
    case PublishError::UploadFailed:
        return "upload failed";
    case PublishError::PlaylistInconsistent:
        return "ffmpeg playlist inconsistent";
    case PublishError::SegmentsLost:
        return "segments lost";
    case PublishError::KeyframeIntervalExceeded:
        return "keyframe interval exceeds the contract";
    }
    return "unknown";
}

Publisher::Publisher(PublisherConfig config, core::ports::IObjectTransfer& store,
                     const core::ports::IClock& clock, LiveWindow window, std::uint32_t epoch,
                     bool resumed)
    : config_(std::move(config)), store_(store), clock_(clock), window_(std::move(window)),
      tracker_(epoch, window_.next_sequence()), epoch_(epoch), resumed_(resumed) {}

std::expected<Publisher, PublishError> Publisher::open(PublisherConfig config,
                                                       core::ports::IObjectTransfer& store,
                                                       const core::ports::IClock& clock,
                                                       std::optional<std::uint32_t>* claimed) {
    std::error_code ec;
    fs::create_directories(config.outbox, ec);
    if (ec) {
        return std::unexpected(PublishError::StoreUnreadable);
    }
    // The newest claim wins: an older packager finds it before its next playlist write and
    // stops. The playlist is read again after the claim, because until then the stream's
    // previous writer may still have been publishing, and continuing from a copy read before
    // it would go backwards. If a newer epoch appeared meanwhile, claim above it.
    std::int64_t floor = 0;
    for (int attempt = 0; attempt < kMaxClaimAttempts; ++attempt) {
        auto before = read_stored(store, config.stream, config.outbox);
        if (!before) {
            return std::unexpected(before.error());
        }
        if (const auto refused = check_resumable(before->playlist, config.window)) {
            return std::unexpected(*refused);
        }
        const auto seen = last_epoch(before->playlist);
        if (!seen) {
            return std::unexpected(seen.error());
        }
        const auto claim = claim_from(store, config, std::max(floor, *seen + 1));
        if (!claim) {
            return std::unexpected(claim.error());
        }
        if (claimed != nullptr) {
            *claimed = *claim;
        }
        auto after = read_stored(store, config.stream, config.outbox);
        if (!after) {
            return std::unexpected(after.error());
        }
        const auto newest = last_epoch(after->playlist);
        if (!newest) {
            return std::unexpected(newest.error());
        }
        if (*newest >= static_cast<std::int64_t>(*claim)) {
            floor = *newest + 1;
            continue;
        }
        if (const auto refused = check_resumable(after->playlist, config.window)) {
            return std::unexpected(*refused);
        }
        std::optional<MediaPlaylist> playlist = std::move(after->playlist);
        if (playlist && !playlist->segments.empty()) {
            LiveWindow window = LiveWindow::resume(config.window, std::move(*playlist));
            return Publisher(std::move(config), store, clock, std::move(window), *claim, true);
        }
        LiveWindow window = LiveWindow::fresh(config.window);
        return Publisher(std::move(config), store, clock, std::move(window), *claim, false);
    }
    return std::unexpected(PublishError::ClaimFailed);
}

void Publisher::begin_epoch(core::WallTime first_media) {
    window_.begin_epoch(first_media);
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

std::expected<void, PublishError> Publisher::mark_ending() {
    const fs::path file = config_.outbox / kEndedByName;
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << epoch_ << '\n';
        if (!out) {
            return std::unexpected(PublishError::UploadFailed);
        }
    }
    return put(file, kEndedByName, claim_type());
}

std::expected<void, PublishError> Publisher::publish_playlist(const MediaPlaylist& playlist) {
    // A packager that started after this one claimed the next epoch. The check comes right
    // before the write and cannot be atomic with it, so a stale writer can still land one
    // playlist that a newer one overwrites at its first; what it cannot do is write objects the
    // newer playlist lists, which carry their epoch in their names.
    const auto newer =
        core::StorageKey::parse(config_.stream.key_prefix() + epoch_claim_name(epoch_ + 1));
    if (!newer) {
        return std::unexpected(PublishError::PlaylistInconsistent);
    }
    const auto seen = store_.size(*newer);
    if (seen) {
        return std::unexpected(PublishError::Superseded);
    }
    // Cannot tell whether the stream has moved on: not writing is the safe answer.
    if (seen.error() != core::ports::StorageError::NotFound) {
        return std::unexpected(PublishError::UploadFailed);
    }
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
        // Not published, and not left for a retry: a segment past the contract stays past it.
        if (window_.exceeds_target(segment.duration)) {
            failure = PublishError::KeyframeIntervalExceeded;
            break;
        }
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
        } else if (!failure || sent.error() == PublishError::Superseded) {
            failure = sent.error();
        }
    }
    if (failure) {
        return std::unexpected(*failure);
    }
    return published;
}

FinishResult Publisher::finish(std::string_view ffmpeg_playlist) {
    FinishResult result;
    if (const auto pumped = pump(ffmpeg_playlist); !pumped) {
        result.problem = pumped.error();
    }
    if (result.problem == PublishError::Superseded || window_.playlist().segments.empty()) {
        return result;
    }
    if (const auto marked = mark_ending(); !marked) {
        result.problem = marked.error();
        return result;
    }
    if (const auto ended = publish_playlist(window_.ended()); ended) {
        result.ended = true;
    } else {
        result.problem = ended.error();
    }
    return result;
}

} // namespace live
