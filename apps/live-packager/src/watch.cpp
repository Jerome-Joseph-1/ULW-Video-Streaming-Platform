#include "watch.hpp"

#include "config.hpp"
#include "log.hpp"
#include "segment_tracker.hpp"

namespace live {

namespace {

// A source that keyframes every segment length completes a segment each one; five without a
// segment tolerate keyframes up to four lengths apart, and a source further out than that
// produces segments no player will take.
constexpr std::uint32_t kStallSegments = 5;
// Progress is logged this often, so a 12 hour stream writes hundreds of lines, not tens of
// thousands.
constexpr std::uint64_t kLogEverySegments = 30;
constexpr std::chrono::seconds kFailureLogEvery{10};

} // namespace

WatchLimits watch_limits(std::uint32_t segment_seconds, std::uint32_t listed_segments) noexcept {
    // ffmpeg lists twice the window (config.hpp), so an uploader that gives up after one
    // window's worth of segments has not yet had one dropped from the list under it.
    return {.upload_patience =
                std::chrono::seconds(std::uint64_t{listed_segments} / 2 * segment_seconds),
            .stall = std::chrono::seconds(std::uint64_t{kStallSegments} * segment_seconds)};
}

Watch::Watch(Publisher& publisher, const WatchLimits& limits, core::MonoTime start)
    : publisher_(publisher), limits_(limits), last_growth_(start) {}

Verdict Watch::look(const std::optional<std::string>& playlist, core::MonoTime now) {
    if (playlist) {
        if (const auto end = listed_end(*playlist); end && *end > listed_end_) {
            listed_end_ = *end;
            last_growth_ = now;
        }
        if (const auto verdict = publish(*playlist, now); verdict != Verdict::Healthy) {
            return verdict;
        }
    }
    if (now - last_growth_ >= limits_.stall) {
        log("no segment finished for {} s: the publisher sent nothing, or its keyframes are "
            "further apart than the segment length",
            limits_.stall.count());
        return Verdict::Stalled;
    }
    return Verdict::Healthy;
}

Verdict Watch::publish(const std::string& playlist, core::MonoTime now) {
    const auto pumped = publisher_.pump(playlist);
    if (pumped) {
        failing_since_.reset();
        const std::uint64_t before = published_;
        published_ += *pumped;
        if (*pumped != 0 &&
            (before == 0 || published_ / kLogEverySegments != before / kLogEverySegments)) {
            log("segment {} published, {} this run", publisher_.window().next_sequence() - 1,
                published_);
        }
        return Verdict::Healthy;
    }
    last_error_ = pumped.error();
    if (!last_failure_log_ || now - *last_failure_log_ >= kFailureLogEvery) {
        log("publish: {}", to_string(pumped.error()));
        last_failure_log_ = now;
    }
    if (pumped.error() == PublishError::Superseded) {
        return Verdict::Superseded;
    }
    if (pumped.error() != PublishError::UploadFailed) {
        return Verdict::Broken;
    }
    if (!failing_since_) {
        failing_since_ = now;
    } else if (now - *failing_since_ >= limits_.upload_patience) {
        log("publish: giving up after {} s of failed uploads", limits_.upload_patience.count());
        return Verdict::StoreGivenUp;
    }
    return Verdict::Healthy;
}

} // namespace live
