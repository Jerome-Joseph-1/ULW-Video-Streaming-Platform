#include "infra/ffmpeg/recording_remux.hpp"

#include "exit_code.hpp"
#include "recording_verdict.hpp"

#include <csignal>
#include <gtest/gtest.h>

namespace {

using infra::ffmpeg::ChildExit;
using infra::ffmpeg::Ending;
using infra::ffmpeg::RemuxFailure;

ChildExit exited(int code, int signal = 0, Ending ending = Ending::Exited) {
    return ChildExit{.exit_code = code,
                     .signal = signal,
                     .ending = ending,
                     .wall = {},
                     .peak_rss_kib = 0,
                     .stderr_tail = "last words\n",
                     .stderr_bytes = 11};
}

RemuxFailure kind_of(const ChildExit& child) {
    const auto verdict = infra::ffmpeg::recording_verdict(child, "ffmpeg");
    EXPECT_FALSE(verdict);
    return verdict ? RemuxFailure::Refused : verdict.error().kind;
}

TEST(RecordingVerdict, OnlyFfmpegsOwnErrorStatusRefusesTheInput) {
    EXPECT_TRUE(infra::ffmpeg::recording_verdict(exited(0), "ffmpeg"));
    EXPECT_EQ(kind_of(exited(1)), RemuxFailure::Refused);
    // 256 minus AVERROR_INVALIDDATA's code, and the exit of an allocation refused under the
    // address-space limit.
    EXPECT_EQ(kind_of(exited(183)), RemuxFailure::Refused);
    EXPECT_EQ(kind_of(exited(244)), RemuxFailure::Refused);
}

TEST(RecordingVerdict, TheSandboxHelpersOwnExitsAndAnInterruptMayPass) {
    for (const int code : {infra::ffmpeg::kSandboxSetupFailed, infra::ffmpeg::kCannotExecute,
                           infra::ffmpeg::kProgramNotFound, 255}) {
        EXPECT_EQ(kind_of(exited(code)), RemuxFailure::Unavailable) << code;
    }
}

TEST(RecordingVerdict, SignalsAndBudgetsSayNothingAboutTheInput) {
    EXPECT_EQ(kind_of(exited(128 + SIGKILL, SIGKILL)), RemuxFailure::Unavailable);
    EXPECT_EQ(kind_of(exited(128 + SIGSEGV, SIGSEGV)), RemuxFailure::Unavailable);
    EXPECT_EQ(kind_of(exited(128 + SIGSYS, SIGSYS)), RemuxFailure::Unavailable);
    EXPECT_EQ(kind_of(exited(137, 0, Ending::CpuExhausted)), RemuxFailure::Unavailable);
    EXPECT_EQ(kind_of(exited(128 + SIGKILL, SIGKILL, Ending::TimedOut)), RemuxFailure::Unavailable);
    const auto timed_out =
        infra::ffmpeg::recording_verdict(exited(143, SIGTERM, Ending::TimedOut), "ffmpeg");
    ASSERT_FALSE(timed_out);
    EXPECT_NE(timed_out.error().detail.find("wall-clock budget"), std::string::npos);
    EXPECT_EQ(kind_of(exited(143, SIGTERM, Ending::Stopped)), RemuxFailure::Stopped);
}

TEST(RecordingCopyCpu, CoversFourTimesTheMeasuredCostByBytesAndByDuration) {
    EXPECT_EQ(infra::ffmpeg::recording_copy_cpu(0, core::Seconds{0}), core::Seconds{60});
    // 607.5 GB, a 12-hour stream at 100 Mbit/s with its eighth: 608 GB at 20 CPU-s each, and
    // 12 hours of silence at 110 CPU-s each.
    EXPECT_EQ(infra::ffmpeg::recording_copy_cpu(607'500'000'000, core::Seconds{12 * 3600}),
              core::Seconds{60 + (608 * 20) + (12 * 110)});
    // At 1 Mbit/s the duration dominates: 6.075 GB in 12 hours.
    EXPECT_EQ(infra::ffmpeg::recording_copy_cpu(6'075'000'000, core::Seconds{12 * 3600}),
              core::Seconds{60 + (7 * 20) + (12 * 110)});
}

} // namespace
