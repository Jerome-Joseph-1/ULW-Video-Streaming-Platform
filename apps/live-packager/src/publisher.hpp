#pragma once

#include "core/ports/clock.hpp"
#include "core/ports/object_transfer.hpp"

#include "live_window.hpp"
#include "segment_tracker.hpp"
#include "stream_id.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace live {

enum class PublishError : std::uint8_t {
    // The store could not say whether the stream already has a playlist, so a start would risk
    // restarting its numbering.
    StoreUnreadable,
    // The store holds a playlist that is not ours.
    StoredPlaylistInvalid,
    // The stream was ended, and an ended stream is not continued.
    AlreadyEnded,
    // The stored playlist was cut for another segment length. Players hold one target duration
    // for a whole stream, so the length cannot change under them.
    SegmentLengthChanged,
    // The store would not take the claim on this run's epoch.
    ClaimFailed,
    // Another packager claimed a newer epoch of the stream: this one is the stale writer and
    // writes nothing more.
    Superseded,
    // An upload was refused or failed; nothing was lost, and the next pump tries again.
    UploadFailed,
    // The scan found ffmpeg's playlist unusable for good.
    PlaylistInconsistent,
    // ffmpeg's list moved past segments that were never uploaded.
    SegmentsLost,
    // A segment ran longer than the target duration allows: the publisher's keyframes are
    // further apart than the segment length.
    KeyframeIntervalExceeded,
};

[[nodiscard]] std::string_view to_string(PublishError e) noexcept;

// The epoch of the run that ended the stream, written just before the playlist's ENDLIST. That
// run may have published no segment of its own (ended before a publisher came), so the ended
// playlist alone does not name it; whoever records the stream needs it to tell that run's own
// claim from a newer packager's.
inline constexpr std::string_view kEndedByName = "ended_by";

struct PublisherConfig {
    StreamId stream;
    WindowConfig window;
    // ffmpeg's output directory: segments are read from here and deleted once uploaded.
    std::filesystem::path media_dir;
    // The playlist is written here before it is uploaded.
    std::filesystem::path outbox;
};

struct FinishResult {
    // The playlist in the store ends with EXT-X-ENDLIST.
    bool ended = false;
    // Why segments could not all be published first, or why the end could not be.
    std::optional<PublishError> problem;
};

// Uploads what the remuxer finishes: each segment, and after it the playlist that lists it, so
// a viewer never reads a playlist naming an object that is not there yet.
class Publisher {
public:
    // Reads the stream's playlist from the store when an earlier run left one, and continues
    // from it, after claiming an epoch of its own with a create-only put (see epoch()).
    // `claimed`, when given, is set to the epoch this call claimed, also when it then refuses:
    // a stream found ended right after the claim was ended by an older run that is now
    // superseded by this one's claim, and whoever records the stream must know that claim is
    // not a newer run still publishing.
    [[nodiscard]] static std::expected<Publisher, PublishError>
    open(PublisherConfig config, core::ports::IObjectTransfer& store,
         const core::ports::IClock& clock, std::optional<std::uint32_t>* claimed = nullptr);

    // Where the next run of ffmpeg starts numbering.
    [[nodiscard]] std::uint64_t next_sequence() const noexcept { return window_.next_sequence(); }
    // The epoch this run claimed. It names the run's init segment and segments; a packager that
    // finds a newer epoch claimed is the stale one and stops.
    [[nodiscard]] std::uint32_t epoch() const noexcept { return epoch_; }
    // Whether the store already holds a playlist for the stream.
    [[nodiscard]] bool resumed() const noexcept { return resumed_; }

    // Media starts flowing, and its first byte arrived at `first_media`.
    void begin_epoch(core::WallTime first_media);

    // Uploads the segments `ffmpeg_playlist` lists as complete and are not yet uploaded, then
    // the window. Returns the number of segments that became visible.
    [[nodiscard]] std::expected<std::size_t, PublishError> pump(std::string_view ffmpeg_playlist);

    // Uploads what is left, then the window with EXT-X-ENDLIST, whatever went wrong with the
    // uploading: a viewer is left with a playlist that ends, of what is visible. Nothing to end
    // when no segment was ever published, and nothing written once superseded.
    [[nodiscard]] FinishResult finish(std::string_view ffmpeg_playlist);

    [[nodiscard]] const LiveWindow& window() const noexcept { return window_; }

private:
    Publisher(PublisherConfig config, core::ports::IObjectTransfer& store,
              const core::ports::IClock& clock, LiveWindow window, std::uint32_t epoch,
              bool resumed);

    [[nodiscard]] std::expected<void, PublishError>
    put(const std::filesystem::path& file, std::string_view name, const core::ContentType& type);
    [[nodiscard]] std::expected<void, PublishError> publish_playlist(const MediaPlaylist& playlist);
    [[nodiscard]] std::expected<void, PublishError> mark_ending();

    PublisherConfig config_;
    core::ports::IObjectTransfer& store_;
    const core::ports::IClock& clock_;
    LiveWindow window_;
    SegmentTracker tracker_;
    std::uint32_t epoch_;
    bool resumed_;
    // The init segment last uploaded, so each is uploaded once.
    std::string uploaded_init_;
    // The window changed since the playlist was last uploaded.
    bool dirty_ = false;
};

} // namespace live
