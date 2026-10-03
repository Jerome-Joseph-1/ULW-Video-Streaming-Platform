#include "exit_code.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <filesystem>
#include <gtest/gtest.h>
#include <string_view>
#include <utility>

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

TEST(ExitCode, AKillBySeccompIsItsOwnKindNeitherARejectionNorAPlainKill) {
    EXPECT_EQ(killed_by(SIGSYS), TranscodeFailure::SyscallBlocked);
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

// What ffprobe 6.1 prints for a source it cannot open, and for one it cannot decode.
const std::filesystem::path kSource = "/scratch/soak-worker/job-1/source";
const std::filesystem::path kOut = "/scratch/soak-worker/job-1/hls";
constexpr std::string_view kDenied = "/scratch/soak-worker/job-1/source: Permission denied\n";
constexpr std::string_view kUndecodable =
    "/scratch/soak-worker/job-1/source: Invalid data found when processing input\n";

TEST(Refusal, ASourceTheProgramMayNotOpenIsOursNotTheInputs) {
    const std::array ours{kSource};
    EXPECT_TRUE(infra::ffmpeg::refused_our_file(kDenied, ours));
    EXPECT_EQ(infra::ffmpeg::refine(TranscodeFailure::Rejected, 1, kDenied, ours),
              TranscodeFailure::Inaccessible);
}

TEST(Refusal, AnUndecodableSourceIsStillRejected) {
    const std::array ours{kSource};
    EXPECT_FALSE(infra::ffmpeg::refused_our_file(kUndecodable, ours));
    EXPECT_EQ(infra::ffmpeg::refine(TranscodeFailure::Rejected, 1, kUndecodable, ours),
              TranscodeFailure::Rejected);
}

TEST(Refusal, APathTheInputNamesIsNotOurs) {
    // A manifest naming a file the sandbox cannot read: the upload's doing.
    const std::array ours{kSource, kOut};
    EXPECT_FALSE(infra::ffmpeg::refused_our_file("/etc/shadow: Permission denied\n", ours));
    // Nor is a sibling whose name merely starts with ours.
    EXPECT_FALSE(infra::ffmpeg::refused_our_file(
        "/scratch/soak-worker/job-1/source2: Permission denied\n", ours));
}

TEST(Refusal, OnlyTheLastLineExactlyNamingOneOfOursCounts) {
    const std::array ours{kSource, kOut};
    // Every access error's text, on the last line with anything on it.
    EXPECT_TRUE(infra::ffmpeg::refused_our_file(
        "/scratch/soak-worker/job-1/hls: Operation not permitted\r\n\n", ours));
    EXPECT_TRUE(infra::ffmpeg::refused_our_file(
        "/scratch/soak-worker/job-1/hls: Read-only file system", ours));
    // A path under ours, or ours quoted inside a longer line: not how ffprobe reports its input.
    EXPECT_FALSE(infra::ffmpeg::refused_our_file(
        "/scratch/soak-worker/job-1/hls/master.m3u8: Operation not permitted", ours));
    EXPECT_FALSE(infra::ffmpeg::refused_our_file(
        "[hls @ 0x1] Failed to open file '/scratch/soak-worker/job-1/hls': Permission denied",
        ours));
    // Anything said after the refusal: the program went on, so the refusal was not its end.
    EXPECT_FALSE(infra::ffmpeg::refused_our_file(
        "/scratch/soak-worker/job-1/source: Permission denied\nInvalid data found\n", ours));
    // More after the refusal's text on the same line.
    EXPECT_FALSE(infra::ffmpeg::refused_our_file(
        "/scratch/soak-worker/job-1/source: Permission denied (while reading a ref)", ours));
}

TEST(Refusal, OnlyARejectionIsRefined) {
    // A crash or a kill keeps its own disposition whatever the program said before it.
    const std::array ours{kSource};
    EXPECT_EQ(infra::ffmpeg::refine(TranscodeFailure::Crashed, 128 + SIGSEGV, kDenied, ours),
              TranscodeFailure::Crashed);
    EXPECT_EQ(infra::ffmpeg::refine(TranscodeFailure::Sandbox, 125, kDenied, ours),
              TranscodeFailure::Sandbox);
}

// The sandbox helper's refusals of the program it was built to run (docs/adr/0089): a name not
// in its table, or a built-in path it may not execute, which it reports with the path and the
// access error. Those are the host's, as Sandbox, before any refinement looks at the text.
TEST(Refusal, TheHelpersRefusalOfItsBuiltInProgramIsASandboxFailure) {
    const std::array ours{kSource, kOut, std::filesystem::path("/usr/bin")};
    constexpr std::string_view kNotBuiltIn =
        "ulw_sandbox: refusing to run ffmpeg7: not a program this helper was built to run\n";
    constexpr std::string_view kNotExecutable =
        "ulw_sandbox: refusing to run ffmpeg: /usr/bin/ffmpeg: Permission denied\n";
    for (const auto& [code, text] : {std::pair{infra::ffmpeg::kProgramNotFound, kNotBuiltIn},
                                     std::pair{infra::ffmpeg::kCannotExecute, kNotExecutable}}) {
        const auto kind = classify(code, kExited, Ending::Exited);
        ASSERT_EQ(kind, TranscodeFailure::Sandbox) << text;
        EXPECT_EQ(infra::ffmpeg::refine(*kind, code, text, ours), TranscodeFailure::Sandbox)
            << text;
    }
}

TEST(Refusal, FfmpegsExitForAnAccessErrorIsOursWhateverItPrinted) {
    // ffmpeg 6.1 on an unreadable input and on an output directory it may not write.
    const std::array ours{kSource, kOut};
    constexpr std::string_view kInput = "[in#0 @ 0x1] Error opening input: Permission denied\n"
                                        "Error opening input files: Permission denied\n";
    constexpr std::string_view kOutput = "[hls @ 0x1] Failed to open segment 'init.mp4'\n"
                                         "[out#0/hls @ 0x2] Nothing was written into output file\n";
    EXPECT_EQ(infra::ffmpeg::refine(TranscodeFailure::Rejected, 256 - EACCES, kInput, ours),
              TranscodeFailure::Inaccessible);
    EXPECT_EQ(infra::ffmpeg::refine(TranscodeFailure::Rejected, 256 - EACCES, kOutput, ours),
              TranscodeFailure::Inaccessible);
    EXPECT_EQ(infra::ffmpeg::refine(TranscodeFailure::Rejected, 256 - EROFS, kOutput, ours),
              TranscodeFailure::Inaccessible);
    // Invalid data (183) and EINVAL (234) stay the input's.
    EXPECT_EQ(infra::ffmpeg::refine(TranscodeFailure::Rejected, 183, kUndecodable, ours),
              TranscodeFailure::Rejected);
    EXPECT_EQ(infra::ffmpeg::refine(TranscodeFailure::Rejected, 234, "Invalid argument\n", ours),
              TranscodeFailure::Rejected);
}

} // namespace
