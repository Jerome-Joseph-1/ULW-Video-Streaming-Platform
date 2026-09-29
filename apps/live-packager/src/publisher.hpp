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
    // An upload was refused or failed; nothing was lost, and the next pump tries again.
    UploadFailed,
    // The scan found ffmpeg's playlist unusable for good.
    PlaylistInconsistent,
    // ffmpeg's list moved past segments that were never uploaded.
    SegmentsLost,
};

[[nodiscard]] std::string_view to_string(PublishError e) noexcept;

struct PublisherConfig {
    StreamId stream;
    WindowConfig window;
    // ffmpeg's output directory: segments are read from here and deleted once uploaded.
    std::filesystem::path media_dir;
    // The playlist is written here before it is uploaded.
    std::filesystem::path outbox;
};

// Uploads what the remuxer finishes: each segment, and after it the playlist that lists it, so
// a viewer never reads a playlist naming an object that is not there yet.
class Publisher {
public:
    // Reads the stream's playlist from the store when an earlier run left one, and continues
    // from it.
    [[nodiscard]] static std::expected<Publisher, PublishError>
    open(PublisherConfig config, core::ports::IObjectTransfer& store,
         const core::ports::IClock& clock);

    // Where the next run of ffmpeg starts numbering, and the epoch that names its init segment.
    [[nodiscard]] std::uint64_t next_sequence() const noexcept { return window_.next_sequence(); }
    [[nodiscard]] std::uint32_t epoch() const noexcept { return epoch_; }
    // Whether the store already holds a playlist for the stream.
    [[nodiscard]] bool resumed() const noexcept { return resumed_; }

    // Media starts flowing now.
    void begin_epoch();

    // Uploads the segments `ffmpeg_playlist` lists as complete and are not yet uploaded, then
    // the window. Returns the number of segments that became visible.
    [[nodiscard]] std::expected<std::size_t, PublishError> pump(std::string_view ffmpeg_playlist);

    // Uploads what is left, then the window with EXT-X-ENDLIST. Nothing to end when no segment
    // was ever published.
    [[nodiscard]] std::expected<void, PublishError> finish(std::string_view ffmpeg_playlist);

    [[nodiscard]] const LiveWindow& window() const noexcept { return window_; }
    // Segments longer than the target duration seen so far.
    [[nodiscard]] std::uint64_t overlong_segments() const noexcept { return overlong_; }

private:
    Publisher(PublisherConfig config, core::ports::IObjectTransfer& store,
              const core::ports::IClock& clock, LiveWindow window, std::uint32_t epoch,
              bool resumed);

    [[nodiscard]] std::expected<void, PublishError>
    put(const std::filesystem::path& file, std::string_view name, const core::ContentType& type);
    [[nodiscard]] std::expected<void, PublishError> publish_playlist(const MediaPlaylist& playlist);

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
    std::uint64_t overlong_ = 0;
};

} // namespace live
