#include "core/models/ladder.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

using core::choose_ladder;
using core::Rung;

std::vector<std::uint32_t> heights(const std::vector<Rung>& ladder) {
    std::vector<std::uint32_t> out;
    out.reserve(ladder.size());
    for (const Rung& r : ladder) {
        out.push_back(r.height);
    }
    return out;
}

TEST(Ladder, FullHdSourceGetsEveryRungTallestFirst) {
    const auto ladder = choose_ladder(1080);
    ASSERT_EQ(ladder.size(), 3U);
    EXPECT_EQ(ladder[0], (Rung{.name = "1080p", .height = 1080, .video_kbps = 5000}));
    EXPECT_EQ(ladder[1], (Rung{.name = "720p", .height = 720, .video_kbps = 2800}));
    EXPECT_EQ(ladder[2], (Rung{.name = "360p", .height = 360, .video_kbps = 800}));
}

TEST(Ladder, SourcesTallerThanTheTopRungAreScaledDownToIt) {
    EXPECT_EQ(heights(choose_ladder(2160)), (std::vector<std::uint32_t>{1080, 720, 360}));
}

TEST(Ladder, A480pSourceGets360pOnly) {
    EXPECT_EQ(heights(choose_ladder(480)), (std::vector<std::uint32_t>{360}));
}

TEST(Ladder, NoRungIsTallerThanTheSource) {
    for (const std::uint32_t source : {1U, 2U, 3U, 360U, 719U, 720U, 1079U, 1080U}) {
        for (const Rung& r : choose_ladder(source)) {
            EXPECT_LE(r.height, source) << "source " << source;
        }
    }
    EXPECT_EQ(heights(choose_ladder(719)), (std::vector<std::uint32_t>{360}));
    EXPECT_EQ(heights(choose_ladder(1079)), (std::vector<std::uint32_t>{720, 360}));
}

TEST(Ladder, ASourceBelowTheLowestRungKeepsItsOwnEvenHeight) {
    const auto ladder = choose_ladder(241);
    ASSERT_EQ(ladder.size(), 1U);
    EXPECT_EQ(ladder[0].height, 240U);
    EXPECT_EQ(ladder[0].name, "240p");
    // 800 kbps at 360 lines, scaled to 240.
    EXPECT_EQ(ladder[0].video_kbps, 533U);
}

TEST(Ladder, ASingleLineGetsNoRungRatherThanAnUpscaledOne) {
    EXPECT_TRUE(choose_ladder(1).empty());
    EXPECT_TRUE(choose_ladder(0).empty());
    EXPECT_EQ(heights(choose_ladder(2)), (std::vector<std::uint32_t>{2}));
    EXPECT_EQ(heights(choose_ladder(3)), (std::vector<std::uint32_t>{2}));
    EXPECT_GT(choose_ladder(2)[0].video_kbps, 0U);
}

} // namespace
