#pragma once

#include "core/ports/transcoder.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

namespace infra::ffmpeg {

// How a child's run ended, as seen by the process that ran it.
enum class Ending : std::uint8_t {
    // On its own, successfully or not.
    Exited,
    // We killed it at its wall-clock deadline.
    TimedOut,
    // We killed it because the caller's stop token fired.
    Stopped,
    // It failed after its CPU time had reached the limit: the kernel's SIGXCPU, or the SIGKILL
    // one second later for a program that catches SIGXCPU, as ffmpeg does (exit 137).
    CpuExhausted,
};

// The sandbox helper's own failures, numbered as env(1) and chroot(1) number theirs, so they
// cannot be mistaken for anything ffmpeg itself returns.
inline constexpr int kSandboxSetupFailed = 125;
inline constexpr int kCannotExecute = 126;
inline constexpr int kProgramNotFound = 127;

// nullopt for success. `signal` is the signal that killed the child, 0 when it exited with
// `exit_code`. An exit code above 128 without a signal is ffmpeg's own failure (it exits with
// 256 minus an error code: 183 for invalid data, 234 for EINVAL), so it rejects the input.
[[nodiscard]] std::optional<core::ports::TranscodeFailure> classify(int exit_code, int signal,
                                                                    Ending ending) noexcept;

// Whether a line of `stderr_tail` says the program was refused one of `ours` (or a path under
// one): it names the path and ends with the access error's strerror text, as ffmpeg and
// ffprobe print a file they cannot open. A path the input itself names (a manifest's) is not
// ours, so an input cannot pass its own failure off as ours.
[[nodiscard]] bool refused_our_file(std::string_view stderr_tail,
                                    std::span<const std::filesystem::path> ours) noexcept;

// `kind` as classify() gave it, made Inaccessible when the program was refused a file of ours:
// what reads as the input's rejection is then the host's fault. ffprobe exits 1 for every
// error, so for it the evidence is the line naming one of `ours`; ffmpeg 6.1 names neither
// its input nor its outputs on the line with the error ("Error opening input files:
// Permission denied", "Failed to open segment 'init.mp4'"), but exits with 256 minus the
// errno: 243 for EACCES, 226 for EROFS. The source formats it may read (kSourceFormats) name
// no other file, so only its own input and outputs can be refused. EPERM (255) is left out:
// that is also ffmpeg's exit after a signal.
[[nodiscard]] core::ports::TranscodeFailure
refine(core::ports::TranscodeFailure kind, int exit_code, std::string_view stderr_tail,
       std::span<const std::filesystem::path> ours) noexcept;

} // namespace infra::ffmpeg
