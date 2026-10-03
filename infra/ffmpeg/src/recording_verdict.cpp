#include "recording_verdict.hpp"

#include "exit_code.hpp"

#include <string>
#include <utility>

namespace infra::ffmpeg {

namespace {

std::string last_line(std::string_view text) {
    while (text.ends_with('\n') || text.ends_with('\r')) {
        text.remove_suffix(1);
    }
    const std::size_t nl = text.rfind('\n');
    return std::string(nl == std::string_view::npos ? text : text.substr(nl + 1));
}

} // namespace

std::expected<void, RemuxError> recording_verdict(const ChildExit& child,
                                                  std::string_view program) {
    const auto failure = classify(child.exit_code, child.signal, child.ending);
    if (!failure) {
        return {};
    }
    RemuxFailure kind = RemuxFailure::Unavailable;
    switch (*failure) {
    case core::ports::TranscodeFailure::Rejected:
        kind = RemuxFailure::Refused;
        break;
    case core::ports::TranscodeFailure::Stopped:
        return std::unexpected(RemuxError{.kind = RemuxFailure::Stopped, .detail = "stopped"});
    case core::ports::TranscodeFailure::Crashed:
    case core::ports::TranscodeFailure::Killed:
    case core::ports::TranscodeFailure::OverBudget:
    case core::ports::TranscodeFailure::Sandbox:
    case core::ports::TranscodeFailure::SyscallBlocked:
    case core::ports::TranscodeFailure::Unverified:
    case core::ports::TranscodeFailure::Inaccessible:
        break;
    }
    std::string detail = std::string(program) + " exited " + std::to_string(child.exit_code);
    if (child.ending == Ending::TimedOut) {
        detail += " past its wall-clock budget";
    } else if (child.ending == Ending::CpuExhausted) {
        detail += " past its CPU budget";
    }
    const std::string said = last_line(child.stderr_tail);
    if (!said.empty()) {
        detail += ": " + said;
    }
    return std::unexpected(RemuxError{.kind = kind, .detail = std::move(detail)});
}

} // namespace infra::ffmpeg
