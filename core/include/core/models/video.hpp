#pragma once

#include "core/errors/domain_error.hpp"
#include "core/models/ids.hpp"
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
};

// A rejected transition leaves every field untouched, so the caller can report the error without
// reloading the row.
class Video {
public:
    // Counted in bytes so the check needs no Unicode tables. 200 bytes still hold a 100-character
    // title, about what players and cards show before truncating, in two-byte scripts such as
    // Cyrillic or Greek.
    static constexpr std::size_t kMaxTitleBytes = 200;
    // FFmpeg formats a log line into 1024 bytes, prefix included, so one full diagnostic fits.
    static constexpr std::size_t kMaxFailureReasonBytes = 1000;

    [[nodiscard]] static std::expected<Video, DomainError> create(VideoId id, const UserId& owner,
                                                                  std::string title);
    [[nodiscard]] static std::expected<Video, DomainError> rehydrate(const VideoRecord& record);

    [[nodiscard]] std::expected<void, DomainError> start_upload() noexcept;
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
