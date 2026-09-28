#include "exit_code.hpp"

#include <csignal>

namespace infra::ffmpeg {

using core::ports::TranscodeFailure;

std::optional<TranscodeFailure> classify(int exit_code, Ending ending) noexcept {
    // A child that we signalled reports that signal, which says nothing about the input.
    switch (ending) {
    case Ending::Stopped:
        return TranscodeFailure::Stopped;
    case Ending::TimedOut:
        return TranscodeFailure::OverBudget;
    case Ending::Exited:
        break;
    }
    constexpr int kSignalled = 128;
    if (exit_code == 0) {
        return std::nullopt;
    }
    if (exit_code >= kSandboxSetupFailed && exit_code <= kProgramNotFound) {
        return TranscodeFailure::Sandbox;
    }
    if (exit_code == kSignalled + SIGSEGV) {
        return TranscodeFailure::Crashed;
    }
    // RLIMIT_CPU sends SIGXCPU at the soft limit.
    if (exit_code == kSignalled + SIGXCPU) {
        return TranscodeFailure::OverBudget;
    }
    if (exit_code > kSignalled) {
        return TranscodeFailure::Killed;
    }
    return TranscodeFailure::Rejected;
}

} // namespace infra::ffmpeg
