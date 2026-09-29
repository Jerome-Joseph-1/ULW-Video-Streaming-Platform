#include "live_window.hpp"

#include <utility>

namespace live {

LiveWindow LiveWindow::fresh(const WindowConfig& config) {
    MediaPlaylist playlist;
    playlist.target_seconds = config.target_seconds;
    return {config, std::move(playlist)};
}

LiveWindow LiveWindow::resume(const WindowConfig& config, MediaPlaylist previous) {
    previous.ended = false;
    if (previous.target_seconds == 0) {
        previous.target_seconds = config.target_seconds;
    }
    LiveWindow window(config, std::move(previous));
    window.trim();
    return window;
}

void LiveWindow::begin_epoch(core::WallTime start) {
    next_start_ = std::chrono::time_point_cast<Micros>(start);
    break_before_next_ = !playlist_.segments.empty();
}

void LiveWindow::add(std::string uri, Micros duration, std::string init) {
    Segment segment{.uri = std::move(uri),
                    .duration = duration,
                    .init = std::move(init),
                    .discontinuity = std::exchange(break_before_next_, false),
                    .program_date_time = std::nullopt};
    if (next_start_) {
        segment.program_date_time = *next_start_;
        *next_start_ += duration;
    }
    playlist_.segments.push_back(std::move(segment));
    trim();
}

bool LiveWindow::exceeds_target(Micros duration) const noexcept {
    constexpr std::int64_t kMicrosPerSecond = 1'000'000;
    // Rounded to the nearest second, halves up.
    const std::int64_t seconds = (duration.count() + (kMicrosPerSecond / 2)) / kMicrosPerSecond;
    return seconds > static_cast<std::int64_t>(playlist_.target_seconds);
}

MediaPlaylist LiveWindow::ended() const {
    MediaPlaylist done = playlist_;
    done.ended = true;
    return done;
}

void LiveWindow::trim() {
    std::size_t drop = 0;
    while (playlist_.segments.size() - drop > max_segments_) {
        // A discontinuity leaving the window moves the discontinuity sequence on, which is
        // how a player counting them keeps its place.
        if (playlist_.segments[drop].discontinuity) {
            ++playlist_.discontinuity_sequence;
        }
        ++drop;
    }
    if (drop != 0) {
        playlist_.segments.erase(playlist_.segments.begin(),
                                 playlist_.segments.begin() + static_cast<std::ptrdiff_t>(drop));
        playlist_.media_sequence += drop;
    }
}

} // namespace live
