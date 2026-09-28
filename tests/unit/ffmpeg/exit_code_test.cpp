#include "exit_code.hpp"

#include <csignal>
#include <gtest/gtest.h>

namespace {

using core::ports::TranscodeFailure;
using infra::ffmpeg::classify;
using infra::ffmpeg::Ending;

constexpr int signalled(int sig) {
    return 128 + sig;
}

TEST(ExitCode, ZeroIsSuccess) {
    EXPECT_EQ(classify(0, Ending::Exited), std::nullopt);
}

TEST(ExitCode, FfmpegsOwnErrorsRejectTheInput) {
    EXPECT_EQ(classify(1, Ending::Exited), TranscodeFailure::Rejected);
    EXPECT_EQ(classify(69, Ending::Exited), TranscodeFailure::Rejected);
}

TEST(ExitCode, SegmentationFaultIs139AndMeansCrashed) {
    EXPECT_EQ(signalled(SIGSEGV), 139);
    EXPECT_EQ(classify(139, Ending::Exited), TranscodeFailure::Crashed);
}

TEST(ExitCode, AnyOtherSignalAbove128IsAKill) {
    EXPECT_EQ(classify(signalled(SIGKILL), Ending::Exited), TranscodeFailure::Killed);
    EXPECT_EQ(classify(signalled(SIGABRT), Ending::Exited), TranscodeFailure::Killed);
    EXPECT_EQ(classify(signalled(SIGBUS), Ending::Exited), TranscodeFailure::Killed);
    // ffmpeg's own exit after it caught SIGTERM or SIGINT from someone else.
    EXPECT_EQ(classify(255, Ending::Exited), TranscodeFailure::Killed);
}

TEST(ExitCode, TheCpuLimitIsABudgetNotAKill) {
    EXPECT_EQ(classify(signalled(SIGXCPU), Ending::Exited), TranscodeFailure::OverBudget);
}

TEST(ExitCode, TheSandboxHelpersOwnFailuresAreSandboxFailures) {
    EXPECT_EQ(classify(infra::ffmpeg::kSandboxSetupFailed, Ending::Exited),
              TranscodeFailure::Sandbox);
    EXPECT_EQ(classify(infra::ffmpeg::kCannotExecute, Ending::Exited), TranscodeFailure::Sandbox);
    EXPECT_EQ(classify(infra::ffmpeg::kProgramNotFound, Ending::Exited), TranscodeFailure::Sandbox);
    EXPECT_EQ(classify(128, Ending::Exited), TranscodeFailure::Rejected);
}

TEST(ExitCode, WhatWeKilledIsClassifiedByWhyWeKilledIt) {
    // SIGTERM or SIGKILL from us says nothing about the input.
    EXPECT_EQ(classify(signalled(SIGTERM), Ending::Stopped), TranscodeFailure::Stopped);
    EXPECT_EQ(classify(signalled(SIGKILL), Ending::TimedOut), TranscodeFailure::OverBudget);
    EXPECT_EQ(classify(0, Ending::Stopped), TranscodeFailure::Stopped);
}

} // namespace
