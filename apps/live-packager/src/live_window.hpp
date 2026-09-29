#pragma once

#include "media_playlist.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace live {

struct WindowConfig {
    std::uint32_t target_seconds = 0;
    std::size_t max_segments = 0;
};

// The playlist viewers are served, grown one segment at a time. Its media sequence only ever
// grows, and a segment's place in the stream is media_sequence + its index, so a segment keeps
// its number as the window slides past it.
class LiveWindow {
public:
    [[nodiscard]] static LiveWindow fresh(const WindowConfig& config);
    // Continues a playlist an earlier run published. Its target duration stays: players hold
    // one for the whole stream (RFC 8216, section 4.4.3.1).
    [[nodiscard]] static LiveWindow resume(const WindowConfig& config, MediaPlaylist previous);

    // Media starts flowing at `start`. On a window that already holds segments the timeline
    // breaks there: another run wrote them, with its own timestamps and init segment.
    void begin_epoch(core::WallTime start);

    void add(std::string uri, Micros duration, std::string init);

    // The sequence number the next segment will have.
    [[nodiscard]] std::uint64_t next_sequence() const noexcept {
        return playlist_.media_sequence + playlist_.segments.size();
    }
    // Does `duration` round to more than the target duration? Players may refuse such a
    // segment, so it means the publisher's keyframes are further apart than the target.
    [[nodiscard]] bool exceeds_target(Micros duration) const noexcept;
    [[nodiscard]] const MediaPlaylist& playlist() const noexcept { return playlist_; }
    [[nodiscard]] MediaPlaylist ended() const;

private:
    LiveWindow(const WindowConfig& config, MediaPlaylist playlist) noexcept
        : max_segments_(config.max_segments), playlist_(std::move(playlist)) {}

    void trim();

    std::size_t max_segments_;
    MediaPlaylist playlist_;
    // Where the next segment starts on the wall clock; set by begin_epoch, then advanced by
    // each segment's duration, so the tags never jump between two segments of one run.
    std::optional<std::chrono::sys_time<Micros>> next_start_;
    bool break_before_next_ = false;
};

} // namespace live
