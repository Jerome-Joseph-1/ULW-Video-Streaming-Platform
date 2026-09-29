#pragma once

#include "core/ports/transcoder.hpp"

#include <cstdint>
#include <optional>

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

} // namespace infra::ffmpeg
