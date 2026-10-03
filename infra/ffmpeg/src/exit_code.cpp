#include "exit_code.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
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

bool refused_our_file(std::string_view stderr_tail,
                      std::span<const std::filesystem::path> ours) noexcept {
    // strerror's text for EACCES, EPERM and EROFS.
    constexpr std::array<std::string_view, 3> kRefusals = {
        "Permission denied", "Operation not permitted", "Read-only file system"};
    const auto blank = [](char c) { return c == '\n' || c == '\r' || c == ' '; };
    while (!stderr_tail.empty() && blank(stderr_tail.back())) {
        stderr_tail.remove_suffix(1);
    }
    const std::size_t nl = stderr_tail.rfind('\n');
    std::string_view line = nl == std::string_view::npos ? stderr_tail : stderr_tail.substr(nl + 1);
    for (const auto& path : ours) {
        const std::string& text = path.native();
        if (text.empty() || !line.starts_with(text) ||
            !line.substr(text.size()).starts_with(": ")) {
            continue;
        }
        const std::string_view said = line.substr(text.size() + 2);
        if (std::ranges::find(kRefusals, said) != kRefusals.end()) {
            return true;
        }
    }
    return false;
}

TranscodeFailure refine(TranscodeFailure kind, int exit_code, std::string_view stderr_tail,
                        std::span<const std::filesystem::path> ours) noexcept {
    constexpr int kFfmpegAccessDenied = 256 - EACCES;
    constexpr int kFfmpegReadOnly = 256 - EROFS;
    if (kind != TranscodeFailure::Rejected) {
        return kind;
    }
    if (exit_code == kFfmpegAccessDenied || exit_code == kFfmpegReadOnly ||
        refused_our_file(stderr_tail, ours)) {
        return TranscodeFailure::Inaccessible;
    }
    return kind;
}

} // namespace infra::ffmpeg
