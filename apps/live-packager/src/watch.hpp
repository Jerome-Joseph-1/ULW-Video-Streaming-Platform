#pragma once

#include "core/util/time.hpp"

#include "publisher.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace live {

struct WatchLimits {
    // Failed uploads are given up on after this long without one succeeding.
    std::chrono::seconds upload_patience{};
    // ffmpeg is given up on after this long without finishing a segment.
    std::chrono::seconds stall{};
};

// Derived from the segment length and the window (see config.hpp) rather than chosen.
[[nodiscard]] WatchLimits watch_limits(std::uint32_t segment_seconds,
                                       std::uint32_t listed_segments) noexcept;

enum class Verdict : std::uint8_t {
    Healthy,
    // The store kept refusing uploads for the whole patience.
    StoreGivenUp,
    // ffmpeg finished no segment for the whole stall limit: the publisher sent nothing, or its
    // keyframes are too far apart, whatever the store is doing.
    Stalled,
    // The publish itself broke: playlists that disagree, segments lost, a segment longer than
    // the contract.
    Broken,
    // A newer packager of this stream took over.
    Superseded,
};

// Follows one run of publishing, one look at ffmpeg's playlist at a time, and says when it
// has failed for good. The two clocks it keeps are separate on purpose: the stall clock moves
// with ffmpeg's playlist growing, the upload clock with the store answering. A store outage
// therefore runs on the upload clock alone, however long the publisher has been sending.
class Watch {
public:
    Watch(Publisher& publisher, const WatchLimits& limits, core::MonoTime start);

    // `playlist` is ffmpeg's playlist as it is now, nullopt while there is none.
    [[nodiscard]] Verdict look(const std::optional<std::string>& playlist, core::MonoTime now);

    // What the last look that did not find all well had to say.
    [[nodiscard]] std::optional<PublishError> last_error() const noexcept { return last_error_; }

private:
    Verdict publish(const std::string& playlist, core::MonoTime now);

    Publisher& publisher_;
    WatchLimits limits_;
    core::MonoTime last_growth_;
    std::uint64_t listed_end_ = 0;
    std::uint64_t published_ = 0;
    std::optional<core::MonoTime> failing_since_;
    std::optional<core::MonoTime> last_failure_log_;
    std::optional<PublishError> last_error_;
};

} // namespace live
