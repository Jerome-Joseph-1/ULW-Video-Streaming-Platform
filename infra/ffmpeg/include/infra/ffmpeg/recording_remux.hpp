#pragma once

#include "core/ports/clock.hpp"
#include "core/util/time.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace infra::ffmpeg {

struct RecordingRemuxConfig {
    // The ulw_sandbox helper the children are started through (ADR-0025).
    std::filesystem::path sandbox;
    std::string ffmpeg = "ffmpeg";
    std::string ffprobe = "ffprobe";
    // PATH for the children, which get no other environment.
    std::string search_path;
};

enum class RecordingInput : std::uint8_t {
    // One run's init segment followed by its media segments, as the live packager stored them.
    FragmentedMp4,
    // MPEG-TS whose timestamps may jump where one run's pieces end and the next one's begin.
    // ffmpeg treats a jump in a transport stream as a discontinuity and carries the timeline
    // on across it, so the output's timestamps rise throughout and its duration is the sum.
    MpegTs,
};

// The audio a run carries, as far as a stand-in for missing audio must match it.
struct AudioFormat {
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;

    friend bool operator==(const AudioFormat&, const AudioFormat&) = default;
};

struct RecordingRemuxJob {
    RecordingInput from = RecordingInput::FragmentedMp4;
    // Becomes the child's stdin. Borrowed: the caller closes its end of the pipe to end the
    // input.
    int input = -1;
    // The child's writable directory, which it has no reason to write: an empty one of its own.
    std::filesystem::path work_dir;
    // The child is stopped past either.
    core::Seconds wall{};
    core::Seconds cpu{};
    // FragmentedMp4 only: the run has no audio and the recording does, so silence of this
    // format is encoded alongside its video, and every run the second stage joins carries the
    // same streams.
    std::optional<AudioFormat> silence;
};

enum class RemuxFailure : std::uint8_t {
    // The child could not be started (the sandbox helper's own exits, 125 to 127), was killed
    // by a signal (the kernel's OOM killer, the syscall filter) or interrupted (ffmpeg's 255),
    // or ran past its wall-clock or CPU budget: on another start, or a fixed deployment, the
    // same input may go through.
    Unavailable,
    // ffmpeg or ffprobe exited on its own with its error status: it read the input and refused
    // it, and does so every time. Running out of its address space is one such exit.
    Refused,
    // The caller's stop token fired.
    Stopped,
};

struct RemuxError {
    RemuxFailure kind = RemuxFailure::Refused;
    // For the log: the child's last words, or why it did not start.
    std::string detail;
};

// CPU time a copy of up to `bytes` of a stream up to `duration` long may take. Measured with
// ffmpeg 6.1, pipe I/O included: on 121 MB of 720p at 8 Mbit/s the fMP4-to-TS copy took 0.47
// CPU-seconds and the TS-to-TS copy 0.60, so at most 5 CPU-seconds per GB; a run without audio
// is given AAC silence, whose encoding is bound by the duration instead, 4.56 CPU-seconds per
// 10 minutes, 27.4 an hour. Four times each (20 per GB, 110 per hour), and a minute for startup
// and probing: 3.8 hours for a 12-hour stream at 100 Mbit/s, 25 minutes for one at 1 Mbit/s.
[[nodiscard]] core::Seconds recording_copy_cpu(std::uint64_t bytes,
                                               core::Seconds duration) noexcept;

// Copies the video and the audio of a recording into MPEG-TS, without decoding them, with
// ffmpeg as a sandboxed child whose stdout is handed to `on_output` as it arrives. Blocks until
// the child has exited.
class RecordingRemuxer {
public:
    RecordingRemuxer(RecordingRemuxConfig config, const core::ports::IClock& clock);

    [[nodiscard]] std::expected<void, RemuxError>
    run(const RecordingRemuxJob& job, const std::function<void(std::string_view)>& on_output,
        const std::stop_token& stop) const;

    // The first audio stream of an init segment; nullopt for a run without audio.
    [[nodiscard]] std::expected<std::optional<AudioFormat>, RemuxError>
    probe_audio(const std::filesystem::path& init, const std::filesystem::path& work_dir,
                const std::stop_token& stop) const;

private:
    RecordingRemuxConfig config_;
    const core::ports::IClock& clock_;
};

} // namespace infra::ffmpeg
