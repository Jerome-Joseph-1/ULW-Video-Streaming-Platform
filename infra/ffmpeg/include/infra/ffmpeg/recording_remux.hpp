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
    // The child could not be started, was killed by a signal (the kernel's OOM killer among
    // them), or ran past its wall-clock or CPU budget: on another start, or a quieter host, the
    // same input may go through.
    Unavailable,
    // ffmpeg or ffprobe exited on its own with an error: it read the input and refused it, and
    // does so every time.
    Refused,
    // The caller's stop token fired.
    Stopped,
};

struct RemuxError {
    RemuxFailure kind = RemuxFailure::Refused;
    // For the log: the child's last words, or why it did not start.
    std::string detail;
};

// CPU time a copy of up to `bytes` may take. Measured with ffmpeg 6.1 on 121 MB of 720p at
// 8 Mbit/s: the fMP4-to-TS copy took 0.47 CPU-seconds and the TS-to-TS copy 0.60, pipe I/O
// included, so at most 5 CPU-seconds per GB. Four times that, and a minute for startup and
// probing: 3.4 hours for the 607.5 GB of a 12-hour stream at 100 Mbit/s.
[[nodiscard]] core::Seconds recording_copy_cpu(std::uint64_t bytes) noexcept;

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
