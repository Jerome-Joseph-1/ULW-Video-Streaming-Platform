#pragma once

#include "core/errors/domain_error.hpp"
#include "core/models/ids.hpp"
#include "core/models/visibility.hpp"
#include "core/util/time.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace core {

enum class VideoState : std::uint8_t { Init, Uploading, Processing, Ready, Failed };

// Where a processing video's transcode is (ADR-0101): waiting for a worker (a first attempt, or
// a retry's backoff), or one is on it.
enum class TranscodeStage : std::uint8_t { Queued, Transcoding };

struct TranscodeProgress {
    TranscodeStage stage = TranscodeStage::Queued;
    // 0..99 while transcoding, from the worker's last report; 0 while queued. Only the video
    // becoming ready completes it.
    std::uint8_t percent = 0;

    friend bool operator==(const TranscodeProgress&, const TranscodeProgress&) = default;
};

// A video as the repository stores it. Video::rehydrate decides whether it is consistent.
struct VideoRecord {
    VideoId id;
    UserId owner;
    std::string title;
    VideoState state = VideoState::Init;
    // Bumped by every transition; the repository writes with `WHERE version = <loaded>`.
    std::uint64_t version = 0;
    std::optional<std::string> error_reason;
    std::optional<Millis> duration;
    // Who besides the owner may see it; private until the owner says otherwise (ADR-0097). The
    // initializer lets designated initializers that predate the field leave it out.
    Visibility visibility{}; // NOLINT(readability-redundant-member-init)
    // Read with the video from its transcode job, not part of it: set by the reads that answer
    // clients while the video is processing, nullopt otherwise. Nothing writes it back.
    std::optional<TranscodeProgress> progress{}; // NOLINT(readability-redundant-member-init)
};

// A rejected transition leaves every field untouched, so the caller can report the error without
// reloading the row.
class Video {
public:
    // Bytes, not characters: 200 still hold a 100-character title in Cyrillic or Greek.
    static constexpr std::size_t kMaxTitleBytes = 200;
    // FFmpeg formats a log line into 1024 bytes, prefix included, so one full diagnostic fits.
    static constexpr std::size_t kMaxFailureReasonBytes = 1000;

    [[nodiscard]] static std::expected<Video, DomainError> create(VideoId id, const UserId& owner,
                                                                  std::string title);
    [[nodiscard]] static std::expected<Video, DomainError> rehydrate(const VideoRecord& record);

    [[nodiscard]] std::expected<void, DomainError> start_processing() noexcept;
    [[nodiscard]] std::expected<void, DomainError> mark_ready(Millis duration) noexcept;
    [[nodiscard]] std::expected<void, DomainError> mark_failed(std::string reason) noexcept;

    [[nodiscard]] const VideoId& id() const noexcept { return data_.id; }
    [[nodiscard]] const UserId& owner() const noexcept { return data_.owner; }
    [[nodiscard]] std::string_view title() const noexcept { return data_.title; }
    [[nodiscard]] VideoState state() const noexcept { return data_.state; }
    [[nodiscard]] std::uint64_t version() const noexcept { return data_.version; }
    [[nodiscard]] std::optional<std::string_view> error_reason() const noexcept;
    [[nodiscard]] std::optional<Millis> duration() const noexcept { return data_.duration; }

private:
    explicit Video(VideoRecord data) noexcept : data_(std::move(data)) {}

    [[nodiscard]] std::expected<void, DomainError>
    require_state(std::initializer_list<VideoState> allowed) const noexcept;
    void enter(VideoState next) noexcept;

    VideoRecord data_;
};

} // namespace core
