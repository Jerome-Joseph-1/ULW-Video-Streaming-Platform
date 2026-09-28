#pragma once

#include "core/models/ladder.hpp"
#include "core/util/time.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <stop_token>
#include <string>

namespace core::ports {

// A container's r_frame_rate, kept as the rational it is: 30000/1001 is not 29.97.
struct FrameRate {
    std::uint32_t num = 0;
    std::uint32_t den = 1;

    friend bool operator==(const FrameRate&, const FrameRate&) = default;
};

struct MediaInfo {
    // As displayed, after any rotation the container asks for.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    FrameRate frame_rate;
    Millis duration{};
    bool has_audio = false;
};

enum class TranscodeFailure : std::uint8_t {
    // The decoder or the prober refused the input; running it again changes nothing.
    Rejected,
    // Killed by SIGSEGV. Decoder crashes are sometimes timing-dependent, so one rerun is worth it.
    Crashed,
    // Killed by another signal nobody here sent, such as the kernel's OOM killer.
    Killed,
    // Ran past its wall-clock deadline or its CPU limit.
    OverBudget,
    // The caller's stop token fired.
    Stopped,
    // The sandbox could not be set up or the program could not be started.
    Sandbox,
    // The output exists but failed verification.
    Unverified,
};

struct TranscodeError {
    TranscodeFailure kind = TranscodeFailure::Rejected;
    // As a shell reports it: 128 + the signal number for a child killed by a signal.
    int exit_code = 0;
    // For the log; never shown to a client.
    std::string detail;
};

template <class T> using TranscodeResult = std::expected<T, TranscodeError>;

// Of the child process that did the work.
struct TranscodeStats {
    Millis wall{};
    std::uint64_t peak_rss_kib = 0;
};

class ITranscodeProgress {
public:
    virtual ~ITranscodeProgress() = default;
    // How much of the media has been encoded so far; called many times a minute.
    virtual void on_progress(Millis encoded) noexcept = 0;
};

// Turns one local media file into an HLS fMP4 ladder. Every call blocks until its child
// process has exited, and returns early (Stopped) once `stop` fires.
class ITranscoder {
public:
    virtual ~ITranscoder() = default;
    [[nodiscard]] virtual TranscodeResult<MediaInfo> probe(const std::filesystem::path& input,
                                                           std::stop_token stop) = 0;
    // Writes out_dir/master.m3u8 and, per rung, out_dir/<name>/index.m3u8 with the init and
    // media segments beside it. `ladder` is tallest first.
    [[nodiscard]] virtual TranscodeResult<TranscodeStats>
    run(const std::filesystem::path& input, const std::filesystem::path& out_dir,
        const MediaInfo& media, std::span<const Rung> ladder, ITranscodeProgress& progress,
        std::stop_token stop) = 0;
    // Checks what run() wrote before anything is published: keyframes fall at the same times
    // in every rung, and the master decodes without a single error.
    [[nodiscard]] virtual TranscodeResult<void> verify(const std::filesystem::path& out_dir,
                                                       const MediaInfo& media,
                                                       std::span<const Rung> ladder,
                                                       std::stop_token stop) = 0;
};

} // namespace core::ports
