#include "exit_code.hpp"

#include <csignal>

namespace infra::ffmpeg {

using core::ports::TranscodeFailure;

std::optional<TranscodeFailure> classify(int exit_code, int signal, Ending ending) noexcept {
    // A child that we signalled reports that signal, which says nothing about the input.
    switch (ending) {
    case Ending::Stopped:
        return TranscodeFailure::Stopped;
    case Ending::TimedOut:
    case Ending::CpuExhausted:
        return TranscodeFailure::OverBudget;
    case Ending::Exited:
        break;
    }
    if (signal == SIGSEGV) {
        return TranscodeFailure::Crashed;
    }
    // The syscall filter killed it. Either the input drove the decoder somewhere it has no
    // business, or the allowlist has a gap; the exit code cannot say which, so this is neither
    // a verdict on the file nor a plain kill.
    if (signal == SIGSYS) {
        return TranscodeFailure::SyscallBlocked;
    }
    if (signal != 0) {
        return TranscodeFailure::Killed;
    }
    // ffmpeg's exit after it caught SIGTERM or SIGINT from someone else.
    constexpr int kFfmpegInterrupted = 255;
    if (exit_code == 0) {
        return std::nullopt;
    }
    if (exit_code >= kSandboxSetupFailed && exit_code <= kProgramNotFound) {
        return TranscodeFailure::Sandbox;
    }
    if (exit_code == kFfmpegInterrupted) {
        return TranscodeFailure::Killed;
    }
    return TranscodeFailure::Rejected;
}

} // namespace infra::ffmpeg
