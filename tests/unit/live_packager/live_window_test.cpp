#include "live_window.hpp"

#include <chrono>
#include <gtest/gtest.h>
#include <string>

namespace {

using live::LiveWindow;
using live::MediaPlaylist;
using live::Micros;

constexpr Micros kTwoSeconds{2'000'000};
const core::WallTime kStart{std::chrono::milliseconds{1'790'672'777'431}};

std::string name(std::uint64_t n) {
    return "seg_" + std::to_string(n) + ".m4s";
}

void add_segments(LiveWindow& window, std::uint64_t from, std::uint64_t count,
                  const std::string& init = "init_0.mp4") {
    for (std::uint64_t n = from; n < from + count; ++n) {
        window.add(name(n), kTwoSeconds, init);
    }
}

TEST(LiveWindow, GrowsFromSequenceZeroUntilItHoldsItsMaximum) {
    LiveWindow window = LiveWindow::fresh({.target_seconds = 2, .max_segments = 3});
    window.begin_epoch(kStart);
    add_segments(window, 0, 3);
    EXPECT_EQ(window.playlist().media_sequence, 0U);
    EXPECT_EQ(window.playlist().segments.size(), 3U);
    EXPECT_EQ(window.next_sequence(), 3U);
}

TEST(LiveWindow, SlidingDropsTheOldestSegmentAndAdvancesTheMediaSequenceByOne) {
    LiveWindow window = LiveWindow::fresh({.target_seconds = 2, .max_segments = 3});
    window.begin_epoch(kStart);
    add_segments(window, 0, 5);
    EXPECT_EQ(window.playlist().media_sequence, 2U);
    ASSERT_EQ(window.playlist().segments.size(), 3U);
    EXPECT_EQ(window.playlist().segments.front().uri, name(2));
    EXPECT_EQ(window.playlist().segments.back().uri, name(4));
    EXPECT_EQ(window.next_sequence(), 5U);
}

TEST(LiveWindow, TheMediaSequenceNeverDecreasesWhileSegmentsAreAdded) {
    LiveWindow window = LiveWindow::fresh({.target_seconds = 2, .max_segments = 4});
    window.begin_epoch(kStart);
    std::uint64_t last = 0;
    for (std::uint64_t n = 0; n < 50; ++n) {
        window.add(name(n), kTwoSeconds, "init_0.mp4");
        EXPECT_GE(window.playlist().media_sequence, last);
        EXPECT_EQ(window.next_sequence(), n + 1);
        last = window.playlist().media_sequence;
    }
}

TEST(LiveWindow, ProgramDateTimesRunOnFromTheStartByEachDuration) {
    LiveWindow window = LiveWindow::fresh({.target_seconds = 2, .max_segments = 5});
    window.begin_epoch(kStart);
    window.add(name(0), kTwoSeconds, "init_0.mp4");
    window.add(name(1), Micros{1'966'667}, "init_0.mp4");
    window.add(name(2), kTwoSeconds, "init_0.mp4");
    const auto& segments = window.playlist().segments;
    EXPECT_EQ(segments[0].program_date_time, kStart);
    EXPECT_EQ(segments[1].program_date_time, kStart + std::chrono::seconds(2));
    EXPECT_EQ(segments[2].program_date_time, kStart + std::chrono::microseconds(3'966'667));
}

TEST(LiveWindow, TheFirstSegmentOfAFreshStreamHasNoDiscontinuity) {
    LiveWindow window = LiveWindow::fresh({.target_seconds = 2, .max_segments = 5});
    window.begin_epoch(kStart);
    add_segments(window, 0, 2);
    EXPECT_FALSE(window.playlist().segments[0].discontinuity);
    EXPECT_FALSE(window.playlist().segments[1].discontinuity);
}

MediaPlaylist earlier_run(std::uint64_t media_sequence, std::size_t count,
                          std::uint64_t discontinuity_sequence = 0) {
    MediaPlaylist playlist{.target_seconds = 2,
                           .media_sequence = media_sequence,
                           .discontinuity_sequence = discontinuity_sequence,
                           .ended = false,
                           .segments = {}};
    for (std::size_t i = 0; i < count; ++i) {
        playlist.segments.push_back({.uri = name(media_sequence + i),
                                     .duration = kTwoSeconds,
                                     .init = "init_0.mp4",
                                     .discontinuity = false,
                                     .program_date_time = kStart});
    }
    return playlist;
}

TEST(LiveWindow, ResumingContinuesTheSequenceWhereTheEarlierRunStopped) {
    const LiveWindow window =
        LiveWindow::resume({.target_seconds = 2, .max_segments = 6}, earlier_run(20, 6));
    EXPECT_EQ(window.next_sequence(), 26U);
    EXPECT_EQ(window.playlist().media_sequence, 20U);
}

TEST(LiveWindow, TheFirstSegmentAfterAResumeStartsANewTimelineAndTheRestDoNot) {
    LiveWindow window =
        LiveWindow::resume({.target_seconds = 2, .max_segments = 10}, earlier_run(20, 6));
    window.begin_epoch(kStart + std::chrono::minutes(5));
    window.add(name(26), kTwoSeconds, "init_1.mp4");
    window.add(name(27), kTwoSeconds, "init_1.mp4");
    const auto& segments = window.playlist().segments;
    ASSERT_EQ(segments.size(), 8U);
    EXPECT_FALSE(segments[5].discontinuity);
    EXPECT_TRUE(segments[6].discontinuity);
    EXPECT_FALSE(segments[7].discontinuity);
    EXPECT_EQ(segments[6].program_date_time, kStart + std::chrono::minutes(5));
    EXPECT_EQ(segments[7].program_date_time,
              kStart + std::chrono::minutes(5) + std::chrono::seconds(2));
}

TEST(LiveWindow, TheDiscontinuitySequenceAdvancesWhenADiscontinuityLeavesTheWindow) {
    LiveWindow window =
        LiveWindow::resume({.target_seconds = 2, .max_segments = 3}, earlier_run(20, 3, 4));
    window.begin_epoch(kStart);
    add_segments(window, 23, 1, "init_1.mp4");
    EXPECT_EQ(window.playlist().discontinuity_sequence, 4U);
    add_segments(window, 24, 3, "init_1.mp4");
    // Segment 23 carried the discontinuity and has slid out.
    EXPECT_EQ(window.playlist().media_sequence, 24U);
    EXPECT_EQ(window.playlist().discontinuity_sequence, 5U);
}

TEST(LiveWindow, ResumingIntoASmallerWindowDropsTheOldestAndKeepsTheSequenceAligned) {
    const LiveWindow window =
        LiveWindow::resume({.target_seconds = 2, .max_segments = 4}, earlier_run(20, 10));
    EXPECT_EQ(window.playlist().segments.size(), 4U);
    EXPECT_EQ(window.playlist().media_sequence, 26U);
    EXPECT_EQ(window.next_sequence(), 30U);
    EXPECT_EQ(window.playlist().segments.front().uri, name(26));
}

TEST(LiveWindow, ResumingKeepsTheTargetDurationThePlayersAlreadyHold) {
    const LiveWindow window =
        LiveWindow::resume({.target_seconds = 4, .max_segments = 6}, earlier_run(0, 2));
    EXPECT_EQ(window.playlist().target_seconds, 2U);
}

TEST(LiveWindow, ResumingClearsTheEndedFlagOfAWindowThatWasNotEnded) {
    MediaPlaylist previous = earlier_run(0, 2);
    previous.ended = false;
    EXPECT_FALSE(
        LiveWindow::resume({.target_seconds = 2, .max_segments = 6}, previous).playlist().ended);
}

TEST(LiveWindow, EndedIsACopyWithEndlistAndTheWindowGoesOn) {
    LiveWindow window = LiveWindow::fresh({.target_seconds = 2, .max_segments = 3});
    window.begin_epoch(kStart);
    add_segments(window, 0, 2);
    EXPECT_TRUE(window.ended().ended);
    EXPECT_EQ(window.ended().segments.size(), 2U);
    EXPECT_FALSE(window.playlist().ended);
}

TEST(LiveWindow, ASegmentExceedsTheTargetWhenItRoundsToMoreSeconds) {
    const LiveWindow window = LiveWindow::fresh({.target_seconds = 2, .max_segments = 3});
    EXPECT_FALSE(window.exceeds_target(Micros{2'000'000}));
    EXPECT_FALSE(window.exceeds_target(Micros{2'499'999}));
    EXPECT_TRUE(window.exceeds_target(Micros{2'500'000}));
    EXPECT_TRUE(window.exceeds_target(Micros{4'000'000}));
    EXPECT_FALSE(window.exceeds_target(Micros{500'000}));
}

} // namespace
