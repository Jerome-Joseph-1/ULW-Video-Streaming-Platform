#include "exit_code.hpp"

#include <csignal>
#include <gtest/gtest.h>

namespace {

using core::ports::TranscodeFailure;
using infra::ffmpeg::classify;
using infra::ffmpeg::Ending;

constexpr int kExited = 0;

// A child killed by `sig`, as run_sandboxed reports it.
std::optional<TranscodeFailure> killed_by(int sig, Ending ending = Ending::Exited) {
    return classify(128 + sig, sig, ending);
}

TEST(ExitCode, ZeroIsSuccess) {
    EXPECT_EQ(classify(0, kExited, Ending::Exited), std::nullopt);
}

TEST(ExitCode, FfmpegsOwnErrorsRejectTheInput) {
    EXPECT_EQ(classify(1, kExited, Ending::Exited), TranscodeFailure::Rejected);
    EXPECT_EQ(classify(69, kExited, Ending::Exited), TranscodeFailure::Rejected);
}

TEST(ExitCode, FfmpegsErrorCodesAbove128AreNotSignals) {
    // 256 - AVERROR_INVALIDDATA's low byte, and 256 - EINVAL: what ffmpeg 6.1 exits with for
    // a corrupt input and for a bad option.
    EXPECT_EQ(classify(183, kExited, Ending::Exited), TranscodeFailure::Rejected);
    EXPECT_EQ(classify(234, kExited, Ending::Exited), TranscodeFailure::Rejected);
    EXPECT_EQ(classify(128 + SIGSEGV, kExited, Ending::Exited), TranscodeFailure::Rejected);
    EXPECT_EQ(classify(128 + SIGKILL, kExited, Ending::Exited), TranscodeFailure::Rejected);
}

TEST(ExitCode, ASegmentationFaultMeansCrashed) {
    EXPECT_EQ(killed_by(SIGSEGV), TranscodeFailure::Crashed);
}

TEST(ExitCode, AnyOtherSignalIsAKill) {
    EXPECT_EQ(killed_by(SIGKILL), TranscodeFailure::Killed);
    EXPECT_EQ(killed_by(SIGABRT), TranscodeFailure::Killed);
    EXPECT_EQ(killed_by(SIGBUS), TranscodeFailure::Killed);
    // ffmpeg's own exit after it caught SIGTERM or SIGINT from someone else.
    EXPECT_EQ(classify(255, kExited, Ending::Exited), TranscodeFailure::Killed);
}

TEST(ExitCode, AKillBySeccompRejectsTheInputInsteadOfRequeueingIt) {
    // The decoder tried a call the filter forbids; the same file makes it try again.
    EXPECT_EQ(killed_by(SIGSYS), TranscodeFailure::Rejected);
}

TEST(ExitCode, ACpuLimitReachedIsABudgetWhateverTheSignal) {
    // ffmpeg catches SIGXCPU; the hard limit's SIGKILL a second later is what ends it.
    EXPECT_EQ(killed_by(SIGKILL, Ending::CpuExhausted), TranscodeFailure::OverBudget);
    EXPECT_EQ(killed_by(SIGXCPU, Ending::CpuExhausted), TranscodeFailure::OverBudget);
    // With CPU time to spare, the same signals came from someone else.
    EXPECT_EQ(killed_by(SIGKILL), TranscodeFailure::Killed);
    EXPECT_EQ(killed_by(SIGXCPU), TranscodeFailure::Killed);
}

TEST(ExitCode, TheSandboxHelpersOwnFailuresAreSandboxFailures) {
    EXPECT_EQ(classify(infra::ffmpeg::kSandboxSetupFailed, kExited, Ending::Exited),
              TranscodeFailure::Sandbox);
    EXPECT_EQ(classify(infra::ffmpeg::kCannotExecute, kExited, Ending::Exited),
              TranscodeFailure::Sandbox);
    EXPECT_EQ(classify(infra::ffmpeg::kProgramNotFound, kExited, Ending::Exited),
              TranscodeFailure::Sandbox);
    EXPECT_EQ(classify(128, kExited, Ending::Exited), TranscodeFailure::Rejected);
}

TEST(ExitCode, WhatWeKilledIsClassifiedByWhyWeKilledIt) {
    // SIGTERM or SIGKILL from us says nothing about the input.
    EXPECT_EQ(killed_by(SIGTERM, Ending::Stopped), TranscodeFailure::Stopped);
    EXPECT_EQ(killed_by(SIGKILL, Ending::TimedOut), TranscodeFailure::OverBudget);
    EXPECT_EQ(classify(0, kExited, Ending::Stopped), TranscodeFailure::Stopped);
}

} // namespace
