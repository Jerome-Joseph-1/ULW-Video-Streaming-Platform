#include "infra/ffmpeg/recording_remux.hpp"

#include "command.hpp"
#include "process.hpp"

#include <chrono>
#include <utility>

namespace infra::ffmpeg {

namespace {

// As for the live remux: copying decodes nothing, and a gigabyte holds ffmpeg's arenas.
constexpr std::uint64_t kCopyAddressSpace = std::uint64_t{1} << 30U;
constexpr std::int64_t kWallPerCpu = 20;

Args recording_remux_args(const std::string& ffmpeg, RecordingInput from) {
    Args args{ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-nostats"};
    switch (from) {
    case RecordingInput::FragmentedMp4:
        // Every stream the run stored: its video and, when it had one, its audio.
        args.insert(args.end(), {"-f", "mp4", "-i", "pipe:0", "-map", "0"});
        break;
    case RecordingInput::MpegTs:
        args.insert(args.end(),
                    {"-f", "mpegts", "-i", "pipe:0", "-map", "0:v:0", "-map", "0:a:0?"});
        break;
    }
    args.insert(args.end(), {"-c", "copy", "-f", "mpegts", "pipe:1"});
    return args;
}

std::string last_line(std::string_view text) {
    while (text.ends_with('\n') || text.ends_with('\r')) {
        text.remove_suffix(1);
    }
    const std::size_t nl = text.rfind('\n');
    return std::string(nl == std::string_view::npos ? text : text.substr(nl + 1));
}

} // namespace

RecordingRemuxer::RecordingRemuxer(LiveRemuxConfig config, const core::ports::IClock& clock)
    : config_(std::move(config)), clock_(clock) {}

std::expected<void, std::string>
RecordingRemuxer::run(const RecordingRemuxJob& job,
                      const std::function<void(std::string_view)>& on_output,
                      const std::stop_token& stop) const {
    const Sandbox sandbox{.helper = config_.sandbox,
                          .environment = {"PATH=" + config_.search_path}};
    const Limits limits{.writable = job.work_dir,
                        .address_space_bytes = kCopyAddressSpace,
                        .cpu = core::Seconds{job.budget.count() / kWallPerCpu},
                        .wall = std::chrono::duration_cast<core::Millis>(job.budget),
                        .file_size_bytes = 0};
    const auto child =
        run_sandboxed(sandbox, limits, recording_remux_args(config_.ffmpeg, job.from), clock_,
                      on_output, stop, job.input);
    if (!child) {
        return std::unexpected(child.error());
    }
    if (child->ending != Ending::Exited || child->exit_code != 0) {
        return std::unexpected("ffmpeg exited " + std::to_string(child->exit_code) + ": " +
                               last_line(child->stderr_tail));
    }
    return {};
}

} // namespace infra::ffmpeg
