#include "live_command.hpp"

#include "core/util/parse.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace infra::ffmpeg {

namespace {
constexpr std::string_view kInitPrefix = "init_";
constexpr std::string_view kInitSuffix = ".mp4";
} // namespace

std::string live_segment_name(std::uint32_t epoch, std::uint64_t sequence) {
    return "seg_" + std::to_string(epoch) + "_" + std::to_string(sequence) + ".m4s";
}

std::uint64_t live_max_file_bytes(std::uint32_t max_kbps, std::uint32_t segment_seconds) noexcept {
    constexpr std::uint64_t kBytesPerKbit = 125;
    constexpr std::uint64_t kHeadroom = 2;
    const std::uint64_t longest_ms = (std::uint64_t{segment_seconds} * 1000) +
                                     static_cast<std::uint64_t>(kSegmentDriftAllowance.count());
    return max_kbps * kBytesPerKbit * longest_ms / 1000 * kHeadroom;
}

LiveProbe live_probe(std::uint32_t max_kbps, std::uint32_t segment_seconds) noexcept {
    // The relay's start and a keyframe that is late on the segment length.
    constexpr std::uint64_t kSlackMs = 1000;
    // The packager's configuration takes 2 to 10 s segments and at most 100 Mbit/s
    // (apps/live-packager config.cpp); anything past that is clamped here, so the most ffmpeg
    // is told to hold is 11 s at 100 Mbit/s, twice: 275 MB, inside its 1 GiB address space.
    constexpr std::uint64_t kMaxSegmentSeconds = 10;
    constexpr std::uint64_t kMaxKbps = 100'000;
    // Never below the megabyte it probed before the window followed the configuration.
    constexpr std::uint64_t kMinBytes = 1'000'000;
    constexpr std::uint64_t kBytesPerKbit = 125;
    constexpr std::uint64_t kHeadroom = 2;
    const std::uint64_t window_ms =
        (std::min<std::uint64_t>(segment_seconds, kMaxSegmentSeconds) * 1000) + kSlackMs;
    const std::uint64_t bytes =
        std::min<std::uint64_t>(max_kbps, kMaxKbps) * kBytesPerKbit * window_ms / 1000 * kHeadroom;
    return {.window = core::Millis{static_cast<core::Millis::rep>(window_ms)},
            .bytes = std::max(bytes, kMinBytes)};
}

std::string live_init_name(std::uint32_t epoch) {
    return std::string(kInitPrefix) + std::to_string(epoch) + std::string(kInitSuffix);
}

std::optional<std::uint32_t> live_init_epoch(std::string_view name) {
    if (!name.starts_with(kInitPrefix) || !name.ends_with(kInitSuffix)) {
        return std::nullopt;
    }
    name.remove_prefix(kInitPrefix.size());
    name.remove_suffix(kInitSuffix.size());
    return core::parse_integer<std::uint32_t>(name);
}

Args live_remux_args(const std::string& ffmpeg, const LiveRemuxJob& job) {
    const LiveProbe probe = live_probe(job.max_kbps, job.segment_seconds);
    constexpr std::int64_t kMicrosPerMilli = 1000;
    return {ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "warning", "-nostats",
            // ffmpeg reads this much of the stream before it writes anything, which delays
            // the first segment; it must still hold the first video keyframe, which comes up
            // to a segment length after the publisher joined (live_probe).
            "-analyzeduration", std::to_string(probe.window.count() * kMicrosPerMilli),
            "-probesize", std::to_string(probe.bytes),
            // Only the container a publisher may send; nothing here may open another file.
            "-f", "mpegts", "-i", "pipe:0", "-map", "0:v:0",
            // A stream without audio is a stream; only video is required.
            "-map", "0:a:0?", "-c", "copy",
            // TS carries AAC as ADTS frames; the mp4 muxer wants the bare frames and refuses
            // the stream without this. Ignored when there is no audio.
            "-bsf:a", "aac_adtstoasc", "-f", "hls", "-hls_time",
            std::to_string(job.segment_seconds), "-hls_list_size",
            std::to_string(job.listed_segments), "-hls_segment_type", "fmp4",
            "-hls_fmp4_init_filename", live_init_name(job.epoch), "-hls_segment_filename",
            (job.out_dir / ("seg_" + std::to_string(job.epoch) + "_%d.m4s")).string(),
            "-start_number", std::to_string(job.first_sequence),
            // Segments are written under a temporary name and renamed once closed, a second
            // signal beside the playlist that a segment file is whole.
            "-hls_flags", "independent_segments+temp_file",
            (job.out_dir / std::string(kLivePlaylist)).string()};
}

} // namespace infra::ffmpeg
