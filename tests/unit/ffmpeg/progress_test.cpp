#include "progress.hpp"

#include <gtest/gtest.h>
#include <optional>
#include <string>

namespace {

using core::Millis;
using infra::ffmpeg::ProgressParser;

// One block as ffmpeg 6.1 writes it with -progress pipe:1 -nostats.
constexpr std::string_view kBlock = "frame=179\n"
                                    "fps=52.66\n"
                                    "stream_0_0_q=-1.0\n"
                                    "bitrate=N/A\n"
                                    "total_size=N/A\n"
                                    "out_time_us=5994667\n"
                                    "out_time_ms=5994667\n"
                                    "out_time=00:00:05.994667\n"
                                    "dup_frames=0\n"
                                    "drop_frames=0\n"
                                    "speed=1.76x\n"
                                    "progress=continue\n";

TEST(ProgressParser, ReportsTheEncodedPositionOncePerBlock) {
    ProgressParser parser;
    EXPECT_EQ(parser.feed(kBlock), Millis{5994});
    EXPECT_FALSE(parser.ended());
}

TEST(ProgressParser, ReadsMicrosecondsNotTheMisnamedMillisecondField) {
    // ffmpeg's out_time_ms carries microseconds too; only out_time_us is trusted.
    ProgressParser parser;
    EXPECT_EQ(parser.feed("out_time_ms=999999999\nout_time_us=2000000\nprogress=continue\n"),
              Millis{2000});
}

TEST(ProgressParser, ToleratesBytesSplitAnywhere) {
    ProgressParser parser;
    std::optional<Millis> seen;
    for (const char c : kBlock) {
        if (const auto at = parser.feed(std::string_view(&c, 1))) {
            EXPECT_FALSE(seen) << "reported twice";
            seen = at;
        }
    }
    EXPECT_EQ(seen, Millis{5994});
}

TEST(ProgressParser, AReportNeedsTheBlockToEnd) {
    ProgressParser parser;
    EXPECT_EQ(parser.feed("out_time_us=4000000\n"), std::nullopt);
    EXPECT_EQ(parser.feed("progress=continue\n"), Millis{4000});
}

TEST(ProgressParser, SeveralBlocksAtOnceReportTheLatest) {
    ProgressParser parser;
    EXPECT_EQ(parser.feed("out_time_us=1000000\nprogress=continue\n"
                          "out_time_us=3000000\nprogress=continue\n"),
              Millis{3000});
}

TEST(ProgressParser, ABlockBeforeTheFirstFrameReportsNothing) {
    ProgressParser parser;
    EXPECT_EQ(parser.feed("out_time_us=N/A\nprogress=continue\n"), std::nullopt);
    EXPECT_EQ(parser.feed("out_time_us=-9223372036854775807\nprogress=continue\n"), std::nullopt);
}

TEST(ProgressParser, TheLastBlockEndsTheStream) {
    ProgressParser parser;
    EXPECT_EQ(parser.feed("out_time_us=60000000\r\nprogress=end\r\n"), Millis{60000});
    EXPECT_TRUE(parser.ended());
}

TEST(ProgressParser, AnOverlongLineIsSkippedWhole) {
    ProgressParser parser;
    const std::string junk(1000, 'x');
    // The tail of the long line must not be read as a line of its own.
    EXPECT_EQ(parser.feed(junk + "out_time_us=7000000\n"), std::nullopt);
    EXPECT_EQ(parser.feed("progress=continue\n"), std::nullopt);
    EXPECT_EQ(parser.feed("out_time_us=8000000\nprogress=continue\n"), Millis{8000});
}

} // namespace
