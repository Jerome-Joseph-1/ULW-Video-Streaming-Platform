#include "infra/ffmpeg/recording_remux.hpp"

#include "core/util/parse.hpp"

#include "command.hpp"
#include "process.hpp"
#include "recording_verdict.hpp"

#include <chrono>
#include <string>
#include <utility>

namespace infra::ffmpeg {

namespace {

// As for the live remux: copying decodes nothing, and a gigabyte holds ffmpeg's arenas.
constexpr std::uint64_t kCopyAddressSpace = std::uint64_t{1} << 30U;
// The children write nothing but their stdout; the smallest limit the helper takes that is
// not "none" keeps a child that tries from filling the disk.
constexpr std::uint64_t kNoFiles = 1;
// Reading one init segment's stream table: seconds at most, on any host.
constexpr core::Seconds kProbeBudget{20};
// ffprobe's answer for one stream is two short lines.
constexpr std::size_t kMaxProbeOutput = 4096;

// Silence as ffmpeg's lavfi source spells a layout. Live sources are mono or stereo; anything
// else gets stereo silence, which the second stage copies and the worker downmixes anyway.
std::string layout_of(std::uint32_t channels) {
    return channels == 1 ? "mono" : "stereo";
}

Args recording_remux_args(const RecordingRemuxJob& job) {
    Args args{std::string(kFfmpeg), "-nostdin", "-hide_banner", "-loglevel", "error", "-nostats"};
    switch (job.from) {
    case RecordingInput::FragmentedMp4:
        args.insert(args.end(), {"-f", "mp4", "-i", "pipe:0"});
        if (job.silence) {
            args.insert(args.end(), {"-f", "lavfi", "-i",
                                     "anullsrc=r=" + std::to_string(job.silence->sample_rate) +
                                         ":cl=" + layout_of(job.silence->channels),
                                     "-map", "0:v:0", "-map", "1:a:0", "-shortest", "-c:v", "copy",
                                     "-c:a", "aac", "-f", "mpegts", "pipe:1"});
            return args;
        }
        // Video first and audio second in every run, so each gets the same PIDs in the TS.
        args.insert(args.end(), {"-map", "0:v:0", "-map", "0:a:0?"});
        break;
    case RecordingInput::MpegTs:
        args.insert(args.end(),
                    {"-f", "mpegts", "-i", "pipe:0", "-map", "0:v:0", "-map", "0:a:0?"});
        break;
    }
    args.insert(args.end(), {"-c", "copy", "-f", "mpegts", "pipe:1"});
    return args;
}

std::optional<std::uint32_t> field(std::string_view text, std::string_view name) {
    const std::string key = std::string(name) + "=";
    const std::size_t at = text.find(key);
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view value = text.substr(at + key.size());
    value = value.substr(0, value.find('\n'));
    return core::parse_integer<std::uint32_t>(value);
}

} // namespace

core::Seconds recording_copy_cpu(std::uint64_t bytes, core::Seconds duration) noexcept {
    constexpr std::uint64_t kBytesPerGb = 1'000'000'000;
    constexpr std::uint64_t kCpuSecondsPerGb = std::uint64_t{5} * 4;
    constexpr std::int64_t kCpuSecondsPerHour = 110;
    constexpr std::int64_t kSecondsPerHour = 3600;
    constexpr std::int64_t kFloor = 60;
    const auto per_bytes = static_cast<std::int64_t>(
        ((bytes / kBytesPerGb) + (bytes % kBytesPerGb != 0 ? 1 : 0)) * kCpuSecondsPerGb);
    const std::int64_t hours = (duration.count() + kSecondsPerHour - 1) / kSecondsPerHour;
    return core::Seconds{kFloor + per_bytes + (hours * kCpuSecondsPerHour)};
}

RecordingRemuxer::RecordingRemuxer(RecordingRemuxConfig config, const core::ports::IClock& clock)
    : config_(std::move(config)), clock_(clock) {}

std::expected<void, RemuxError>
RecordingRemuxer::run(const RecordingRemuxJob& job,
                      const std::function<void(std::string_view)>& on_output,
                      const std::stop_token& stop) const {
    const Sandbox sandbox{
        .helper = config_.sandbox,
        .environment = {"PATH=" + config_.search_path, std::string(kTwoMallocArenas)}};
    const Limits limits{.writable = job.work_dir,
                        .address_space_bytes = kCopyAddressSpace,
                        .cpu = job.cpu,
                        .wall = std::chrono::duration_cast<core::Millis>(job.wall),
                        .file_size_bytes = kNoFiles};
    const auto child = run_sandboxed(sandbox, limits, recording_remux_args(job), clock_, on_output,
                                     stop, job.input);
    if (!child) {
        return std::unexpected(
            RemuxError{.kind = RemuxFailure::Unavailable, .detail = child.error()});
    }
    return recording_verdict(*child, "ffmpeg");
}

std::expected<std::optional<AudioFormat>, RemuxError>
RecordingRemuxer::probe_audio(const std::filesystem::path& init,
                              const std::filesystem::path& work_dir,
                              const std::stop_token& stop) const {
    const Sandbox sandbox{
        .helper = config_.sandbox,
        .environment = {"PATH=" + config_.search_path, std::string(kTwoMallocArenas)}};
    const Limits limits{.writable = work_dir,
                        .address_space_bytes = kCopyAddressSpace,
                        .cpu = kProbeBudget,
                        .wall = std::chrono::duration_cast<core::Millis>(kProbeBudget),
                        .file_size_bytes = kNoFiles};
    std::string out;
    const Args args{std::string(kFfprobe),
                    "-v",
                    "error",
                    "-select_streams",
                    "a:0",
                    "-show_entries",
                    "stream=sample_rate,channels",
                    "-of",
                    "default=noprint_wrappers=1",
                    init.string()};
    const auto child = run_sandboxed(
        sandbox, limits, args, clock_,
        [&out](std::string_view chunk) {
            if (out.size() < kMaxProbeOutput) {
                out.append(chunk.substr(0, kMaxProbeOutput - out.size()));
            }
        },
        stop);
    if (!child) {
        return std::unexpected(
            RemuxError{.kind = RemuxFailure::Unavailable, .detail = child.error()});
    }
    if (auto judged = recording_verdict(*child, "ffprobe"); !judged) {
        return std::unexpected(judged.error());
    }
    const auto rate = field(out, "sample_rate");
    const auto channels = field(out, "channels");
    if (!rate && !channels) {
        return std::nullopt;
    }
    if (!rate || !channels || *rate == 0 || *channels == 0) {
        return std::unexpected(
            RemuxError{.kind = RemuxFailure::Refused, .detail = "unreadable audio stream"});
    }
    return AudioFormat{.sample_rate = *rate, .channels = *channels};
}

} // namespace infra::ffmpeg
