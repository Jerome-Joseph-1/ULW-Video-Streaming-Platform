#include "live_command.hpp"

#include "core/util/parse.hpp"

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
    return {ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "warning", "-nostats",
            // Left to probe, ffmpeg reads 5 s of the stream before it writes anything, and
            // that is 5 s of latency on every viewer. A second holds the first keyframe and
            // the first audio frames of a stream that keyframes every 2 s.
            "-analyzeduration", "1000000", "-probesize", "1000000",
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
