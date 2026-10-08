// RecordingRemuxer on the real ffmpeg in the real sandbox: which ways of failing mean the input
// is bad, and which may pass on another try.
#include "infra/ffmpeg/recording_remux.hpp"
#include "infra/ffmpeg/transcoder.hpp"
#include "os/system_clock.hpp"
#include "os/unique_fd.hpp"

#include "support/temp_dir.hpp"

#include <array>
#include <cstdlib>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

using infra::ffmpeg::RecordingInput;
using infra::ffmpeg::RemuxFailure;

std::string search_path() {
    // Read while no thread exists that could setenv.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* path = std::getenv("PATH");
    return path == nullptr ? "/usr/bin:/bin" : path;
}

class RecordingRemuxTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (const auto refused =
                infra::ffmpeg::check_sandbox(ULW_SANDBOX_BIN, work_.path(), clock_)) {
            GTEST_SKIP() << "this host refuses the sandbox: " << *refused;
        }
        std::array<int, 2> fds{};
        ASSERT_EQ(::pipe2(fds.data(), O_CLOEXEC), 0);
        read_ = os::UniqueFd(fds[0]);
        write_ = os::UniqueFd(fds[1]);
    }

    os::SystemClock clock_;
    ulw::test::TempDir work_{"ulw-recording-remux"};
    infra::ffmpeg::RecordingRemuxer remuxer_{
        {.sandbox = ULW_SANDBOX_BIN, .search_path = search_path()}, clock_};
    os::UniqueFd read_;
    os::UniqueFd write_;
};

TEST_F(RecordingRemuxTest, InputFfmpegCannotReadIsRefusedForGood) {
    constexpr std::string_view kGarbage = "this is not a fragmented mp4 at all";
    ASSERT_EQ(::write(write_.get(), kGarbage.data(), kGarbage.size()),
              static_cast<ssize_t>(kGarbage.size()));
    write_.reset();
    const auto copied = remuxer_.run({.from = RecordingInput::FragmentedMp4,
                                      .input = read_.get(),
                                      .work_dir = work_.path(),
                                      .wall = core::Seconds{60},
                                      .cpu = core::Seconds{60},
                                      .silence = std::nullopt},
                                     [](std::string_view) {}, {});
    ASSERT_FALSE(copied);
    EXPECT_EQ(copied.error().kind, RemuxFailure::Refused) << copied.error().detail;
}

TEST_F(RecordingRemuxTest, ACopyStoppedAtItsWallClockBudgetMayPassAnotherTime) {
    // The input never ends, so the copy runs until its budget stops it.
    const auto copied = remuxer_.run({.from = RecordingInput::MpegTs,
                                      .input = read_.get(),
                                      .work_dir = work_.path(),
                                      .wall = core::Seconds{1},
                                      .cpu = core::Seconds{60},
                                      .silence = std::nullopt},
                                     [](std::string_view) {}, {});
    ASSERT_FALSE(copied);
    EXPECT_EQ(copied.error().kind, RemuxFailure::Unavailable) << copied.error().detail;
    EXPECT_NE(copied.error().detail.find("wall-clock budget"), std::string::npos)
        << copied.error().detail;
}

} // namespace
