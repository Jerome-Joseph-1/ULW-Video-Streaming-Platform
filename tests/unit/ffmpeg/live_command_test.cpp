#include "live_command.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <string>
#include <string_view>

namespace {

using infra::ffmpeg::Args;
using infra::ffmpeg::LiveRemuxJob;

LiveRemuxJob job() {
    return {.input = 7,
            .out_dir = "/scratch/media",
            .segment_seconds = 2,
            .listed_segments = 20,
            .first_sequence = 41,
            .epoch = 3,
            .max_duration = core::Seconds{3600}};
}

std::string after(const Args& args, std::string_view flag) {
    const auto it = std::ranges::find(args, flag);
    return it == args.end() || it + 1 == args.end() ? std::string() : *(it + 1);
}

bool has(const Args& args, std::string_view flag) {
    return std::ranges::find(args, flag) != args.end();
}

TEST(LiveRemuxArgs, CopyTheStreamsWithoutDecodingThem) {
    const Args args = infra::ffmpeg::live_remux_args("ffmpeg", job());
    EXPECT_EQ(after(args, "-c"), "copy");
    EXPECT_FALSE(has(args, "libx264"));
    EXPECT_FALSE(has(args, "-filter_complex"));
    EXPECT_FALSE(has(args, "-vf"));
}

TEST(LiveRemuxArgs, ReadStdinAsMpegtsAndNothingElse) {
    const Args args = infra::ffmpeg::live_remux_args("ffmpeg", job());
    EXPECT_EQ(after(args, "-i"), "pipe:0");
    EXPECT_EQ(after(args, "-f"), "mpegts");
    EXPECT_TRUE(has(args, "-nostdin"));
}

TEST(LiveRemuxArgs, CutSegmentsAtTheTargetDurationIntoFragmentedMp4) {
    const Args args = infra::ffmpeg::live_remux_args("ffmpeg", job());
    EXPECT_EQ(after(args, "-hls_time"), "2");
    EXPECT_EQ(after(args, "-hls_segment_type"), "fmp4");
    EXPECT_EQ(after(args, "-hls_list_size"), "20");
}

TEST(LiveRemuxArgs, ContinueTheNumberingAndNameTheInitSegmentForItsEpoch) {
    const Args args = infra::ffmpeg::live_remux_args("ffmpeg", job());
    EXPECT_EQ(after(args, "-start_number"), "41");
    EXPECT_EQ(after(args, "-hls_fmp4_init_filename"), "init_3.mp4");
    EXPECT_EQ(args.back(), "/scratch/media/index.m3u8");
    EXPECT_EQ(after(args, "-hls_segment_filename"), "/scratch/media/seg_%d.m4s");
}

TEST(LiveRemuxArgs, TheSegmentPatternProducesTheNamesTheTrackerExpects) {
    const std::string pattern =
        after(infra::ffmpeg::live_remux_args("ffmpeg", job()), "-hls_segment_filename");
    const std::string prefix = "/scratch/media/";
    ASSERT_TRUE(pattern.starts_with(prefix));
    std::string expected = pattern.substr(prefix.size());
    expected.replace(expected.find("%d"), 2, "41");
    EXPECT_EQ(expected, infra::ffmpeg::live_segment_name(41));
}

TEST(LiveRemuxArgs, WriteSegmentsUnderATemporaryNameUntilTheyAreClosed) {
    const std::string flags = after(infra::ffmpeg::live_remux_args("ffmpeg", job()), "-hls_flags");
    EXPECT_NE(flags.find("temp_file"), std::string::npos);
    EXPECT_NE(flags.find("independent_segments"), std::string::npos);
}

TEST(LiveRemuxArgs, ConvertAdtsAudioAndKeepAStreamThatHasNone) {
    const Args args = infra::ffmpeg::live_remux_args("ffmpeg", job());
    EXPECT_EQ(after(args, "-bsf:a"), "aac_adtstoasc");
    EXPECT_TRUE(has(args, "0:a:0?"));
}

TEST(LiveRemuxArgs, ProbeForOneSecondNotTheDefaultFive) {
    const Args args = infra::ffmpeg::live_remux_args("ffmpeg", job());
    EXPECT_EQ(after(args, "-analyzeduration"), "1000000");
    EXPECT_EQ(after(args, "-probesize"), "1000000");
}

TEST(LiveInitName, RoundTripsTheEpoch) {
    EXPECT_EQ(infra::ffmpeg::live_init_name(0), "init_0.mp4");
    EXPECT_EQ(infra::ffmpeg::live_init_epoch("init_12.mp4"), 12U);
    EXPECT_EQ(infra::ffmpeg::live_init_epoch(infra::ffmpeg::live_init_name(4'000'000'000U)),
              4'000'000'000U);
}

TEST(LiveInitName, RefusesNamesThatAreNotOurs) {
    for (const char* other :
         {"", "init.mp4", "init_.mp4", "init_x.mp4", "init_1.m4s", "init_-1.mp4", "init_1.mp4x",
          "xinit_1.mp4", "init_99999999999.mp4"}) {
        EXPECT_FALSE(infra::ffmpeg::live_init_epoch(other)) << other;
    }
}

} // namespace
