#include "infra/ffmpeg/live_remux.hpp"

#include "live_command.hpp"
#include "process.hpp"

#include <chrono>
#include <utility>

namespace infra::ffmpeg {

namespace {

constexpr std::uint64_t kGiB = std::uint64_t{1} << 30U;

// Copying moves bytes and decodes nothing. Measured on the test publisher (640x360,
// 1.75 Mbit/s): 0.1 CPU-seconds for 30 s of stream, 0.3% of a core, and 58 MB resident. A
// gigabyte of address space is what ffprobe gets, and holds ffmpeg's arenas with room to spare.
constexpr std::uint64_t kRemuxAddressSpace = kGiB;
// A twentieth of the run's length in CPU seconds: fifteen times the measured cost, so a stream
// at a bitrate several times the test source's still fits.
constexpr std::int64_t kWallPerCpu = 20;

std::string last_line(std::string_view text) {
    while (text.ends_with('\n') || text.ends_with('\r')) {
        text.remove_suffix(1);
    }
    const std::size_t nl = text.rfind('\n');
    return std::string(nl == std::string_view::npos ? text : text.substr(nl + 1));
}

LiveEnd end_of(const ChildExit& child) {
    switch (child.ending) {
    case Ending::Stopped:
        return LiveEnd::Stopped;
    case Ending::TimedOut:
        return LiveEnd::TimedOut;
    case Ending::CpuExhausted:
        return LiveEnd::Failed;
    case Ending::Exited:
        return child.exit_code == 0 ? LiveEnd::InputEnded : LiveEnd::Failed;
    }
    return LiveEnd::Failed;
}

} // namespace

LiveRemuxer::LiveRemuxer(LiveRemuxConfig config, const core::ports::IClock& clock)
    : config_(std::move(config)), clock_(clock) {}

std::expected<LiveRemuxResult, std::string> LiveRemuxer::run(const LiveRemuxJob& job,
                                                             const std::stop_token& stop) const {
    const Sandbox sandbox{.helper = config_.sandbox,
                          .environment = {"PATH=" + config_.search_path}};
    const Limits limits{.writable = job.out_dir,
                        .address_space_bytes = kRemuxAddressSpace,
                        .cpu = core::Seconds{job.max_duration.count() / kWallPerCpu},
                        .wall = std::chrono::duration_cast<core::Millis>(job.max_duration)};
    const auto child = run_sandboxed(
        sandbox, limits, live_remux_args(config_.ffmpeg, job), clock_, [](std::string_view) {},
        stop, job.input);
    if (!child) {
        return std::unexpected(child.error());
    }
    return LiveRemuxResult{.end = end_of(*child),
                           .exit_code = child->exit_code,
                           .signal = child->signal,
                           .wall = child->wall,
                           .peak_rss_kib = child->peak_rss_kib,
                           .detail = last_line(child->stderr_tail)};
}

} // namespace infra::ffmpeg
