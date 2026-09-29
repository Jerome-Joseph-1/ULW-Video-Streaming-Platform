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

// The name of the segment with this sequence number: ffmpeg numbers its segments from
// LiveRemuxJob::first_sequence, so the number is also the segment's place in the stream.
[[nodiscard]] std::string live_segment_name(std::uint64_t sequence);

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
};

// Copies the video and audio of an MPEG-TS stream into fMP4 HLS segments without decoding
// them, with ffmpeg as a sandboxed child. Blocks until the stream ends.
class LiveRemuxer {
public:
    LiveRemuxer(LiveRemuxConfig config, const core::ports::IClock& clock);

    // Fails only when the child could not be started.
    [[nodiscard]] std::expected<LiveRemuxResult, std::string> run(const LiveRemuxJob& job,
                                                                  std::stop_token stop) const;

private:
    LiveRemuxConfig config_;
    const core::ports::IClock& clock_;
};

} // namespace infra::ffmpeg
