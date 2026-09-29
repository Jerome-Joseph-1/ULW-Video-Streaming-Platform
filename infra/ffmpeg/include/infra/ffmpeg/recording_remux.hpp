#pragma once

#include "core/ports/clock.hpp"
#include "core/util/time.hpp"
#include "infra/ffmpeg/live_remux.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>

namespace infra::ffmpeg {

enum class RecordingInput : std::uint8_t {
    // One run's init segment followed by its media segments, as the live packager stored them.
    FragmentedMp4,
    // MPEG-TS whose timestamps may jump where one run's pieces end and the next one's begin.
    // ffmpeg treats a jump in a transport stream as a discontinuity and carries the timeline
    // on across it, so the output's timestamps rise throughout and its duration is the sum.
    MpegTs,
};

struct RecordingRemuxJob {
    RecordingInput from = RecordingInput::FragmentedMp4;
    // Becomes the child's stdin. Borrowed: the caller closes its end of the pipe to end the
    // input.
    int input = -1;
    // The directory the child may write, which it has no reason to.
    std::filesystem::path work_dir;
    // Wall-clock budget; a twentieth of it in CPU time.
    core::Seconds budget{};
};

// Copies the video and the audio of a recording into MPEG-TS, without decoding them, with
// ffmpeg as a sandboxed child whose stdout is handed to `on_output` as it arrives. Blocks until
// the child has exited; fails with ffmpeg's last words when it did not exit 0.
class RecordingRemuxer {
public:
    RecordingRemuxer(LiveRemuxConfig config, const core::ports::IClock& clock);

    [[nodiscard]] std::expected<void, std::string>
    run(const RecordingRemuxJob& job, const std::function<void(std::string_view)>& on_output,
        const std::stop_token& stop) const;

private:
    LiveRemuxConfig config_;
    const core::ports::IClock& clock_;
};

} // namespace infra::ffmpeg
