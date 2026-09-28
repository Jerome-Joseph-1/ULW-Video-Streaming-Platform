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
};

// The sandbox helper's own failures, numbered as env(1) and chroot(1) number theirs, so they
// cannot be mistaken for anything ffmpeg itself returns.
inline constexpr int kSandboxSetupFailed = 125;
inline constexpr int kCannotExecute = 126;
inline constexpr int kProgramNotFound = 127;

// nullopt for success. `exit_code` is the shell's: 128 + the signal for a signalled child.
[[nodiscard]] std::optional<core::ports::TranscodeFailure> classify(int exit_code,
                                                                    Ending ending) noexcept;

} // namespace infra::ffmpeg
