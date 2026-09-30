#pragma once

#include "core/ports/clock.hpp"
#include "core/util/time.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace infra::ffmpeg {

// The playlist ffmpeg keeps in the output directory: it lists a segment only once the segment
// file is closed, which is what makes it the packager's signal that a segment is complete.
inline constexpr std::string_view kLivePlaylist = "index.m3u8";

// The name of the segment with this sequence number in this epoch: ffmpeg numbers its
// segments from LiveRemuxJob::first_sequence, so the number is also the segment's place in the
// stream. The epoch is in the name so that a second writer of the stream, which claims an epoch
// of its own, can never overwrite an object a playlist of ours lists.
[[nodiscard]] std::string live_segment_name(std::uint32_t epoch, std::uint64_t sequence);

// RFC 8216 section 4.3.3.1: EXT-X-TARGETDURATION must be at least every segment's duration
// rounded to the nearest second, so a segment may run up to half a second past the target
// before the playlist would be wrong. That half second is the drift a keyframe interval may
// have from the segment length; past it the contract is broken.
inline constexpr core::Millis kSegmentDriftAllowance{500};

// The most a file in the output directory may grow to: a segment at `max_kbps` running to the
// longest the contract allows, twice over, for the fMP4 boxes and for a segment that is
// still being written while the next keyframe is late.
[[nodiscard]] std::uint64_t live_max_file_bytes(std::uint32_t max_kbps,
                                                std::uint32_t segment_seconds) noexcept;

// The longest segment and the highest bitrate a live stream may be configured with: the
// packager's configuration refuses more, and live_probe caps its arguments at them.
inline constexpr std::uint32_t kLiveMaxSegmentSeconds = 10;
inline constexpr std::uint32_t kLiveMaxKbps = 100'000;

// How much of the stream ffmpeg reads before it writes the init segment (ADR-0057). It needs a
// video keyframe to learn the picture's size, and the publisher sends one every segment length
// (ADR-0046) from wherever it joined, so the window is a segment length plus a second of
// slack for the relay's start and a late keyframe: a shorter window fails a stream whose first
// keyframe comes late, with "dimensions not set". `bytes` is the window at `max_kbps`, twice,
// and never under 1 MB. The arguments are capped at kLiveMaxSegmentSeconds and kLiveMaxKbps,
// so both stay bounded.
struct LiveProbe {
    core::Millis window{};
    std::uint64_t bytes = 0;
};
[[nodiscard]] LiveProbe live_probe(std::uint32_t max_kbps, std::uint32_t segment_seconds) noexcept;

// Each run writes its own init segment: after a restart the codec parameters may differ, and a
// name reused would replace the init segment the window's older segments still point at.
[[nodiscard]] std::string live_init_name(std::uint32_t epoch);
// The epoch a name from live_init_name carries.
[[nodiscard]] std::optional<std::uint32_t> live_init_epoch(std::string_view name);

struct LiveRemuxConfig {
    // The ulw_sandbox helper the child is started through (ADR-0025).
    std::filesystem::path sandbox;
    std::string ffmpeg = "ffmpeg";
    // PATH for the child, which gets no other environment.
    std::string search_path;
};

struct LiveRemuxJob {
    // An open descriptor carrying MPEG-TS, which becomes the child's stdin. Borrowed: the
    // caller closes it once the run is over.
    int input = -1;
    // The only directory the child writes.
    std::filesystem::path out_dir;
    std::uint32_t segment_seconds = 0;
    // Segments ffmpeg keeps in its own playlist. Generous, so that a segment is still listed
    // when the uploader gets to it.
    std::uint32_t listed_segments = 0;
    // Number of the first segment written, so a restarted run continues the numbering.
    std::uint64_t first_sequence = 0;
    std::uint32_t epoch = 0;
    // The bitrate the publisher may send at most, which bounds the size of a segment file.
    std::uint32_t max_kbps = 0;
    // The run is ended at this age, whatever the input is doing.
    core::Seconds max_duration{};
};

enum class LiveEnd : std::uint8_t {
    // The publisher closed its side: the stream is over.
    InputEnded,
    Stopped,
    TimedOut,
    // ffmpeg died or rejected the input.
    Failed,
};

struct LiveRemuxResult {
    LiveEnd end = LiveEnd::Failed;
    int exit_code = 0;
    int signal = 0;
    core::Millis wall{};
    std::uint64_t peak_rss_kib = 0;
    // The last line ffmpeg printed to stderr.
    std::string detail;
    // The probe ffmpeg was given.
    LiveProbe probe;
    // The run failed, and ffmpeg had found no codec parameters for the video in the probe
    // window: no keyframe came within it. Not set for a stream ffmpeg merely could not probe,
    // such as a data stream it leaves unmapped, on a run that went on.
    bool video_unprobed = false;
};

// Copies the video and audio of an MPEG-TS stream into fMP4 HLS segments without decoding
// them, with ffmpeg as a sandboxed child. Blocks until the stream ends.
class LiveRemuxer {
public:
    LiveRemuxer(LiveRemuxConfig config, const core::ports::IClock& clock);

    // Fails only when the child could not be started.
    [[nodiscard]] std::expected<LiveRemuxResult, std::string>
    run(const LiveRemuxJob& job, const std::stop_token& stop) const;

private:
    LiveRemuxConfig config_;
    const core::ports::IClock& clock_;
};

} // namespace infra::ffmpeg
