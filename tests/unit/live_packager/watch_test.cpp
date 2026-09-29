#include "config.hpp"
#include "support.hpp"
#include "support/fake_clock.hpp"
#include "support/temp_dir.hpp"
#include "watch.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <optional>
#include <string>

namespace {

namespace fs = std::filesystem;
using live::Verdict;
using ulw::test::ffmpeg_playlist;

// One run of publishing against a store that can be made to fail, with ffmpeg simulated by
// a playlist that grows a segment every two seconds, and the clock advanced a second at a time.
class WatchTest : public ::testing::Test {
protected:
    static constexpr std::uint32_t kSegment = 2;
    static constexpr std::size_t kWindow = 10;

    WatchTest() : store(root.path() / "store") { fs::create_directories(media()); }

    [[nodiscard]] fs::path media() const { return root.path() / "media"; }

    void start() {
        auto opened =
            live::Publisher::open({.stream = *live::StreamId::parse("show"),
                                   .window = {.target_seconds = kSegment, .max_segments = kWindow},
                                   .media_dir = media(),
                                   .outbox = root.path() / "outbox"},
                                  store, clock);
        ASSERT_TRUE(opened);
        publisher.emplace(std::move(*opened));
        publisher->begin_epoch(clock.wall_now());
        watch.emplace(*publisher, live::watch_limits(kSegment, live::listed_segments(kWindow)),
                      clock.now());
        ulw::test::write_file(media() / "init_0.mp4");
    }

    // ffmpeg lists a new segment every two seconds of `elapsed_s`, up to `until_second`, and
    // keeps twice the window in its list.
    [[nodiscard]] std::optional<std::string> ffmpeg_list(int now_s, int until_second) {
        const int listed = std::min(now_s, until_second) / static_cast<int>(kSegment);
        while (written < listed) {
            ulw::test::write_file(media() / ("seg_0_" + std::to_string(written) + ".m4s"));
            ++written;
        }
        if (listed == 0) {
            return std::nullopt;
        }
        const int kept = std::min<int>(listed, static_cast<int>(live::listed_segments(kWindow)));
        return ffmpeg_playlist(0, static_cast<std::uint64_t>(listed - kept),
                               static_cast<std::uint64_t>(kept), "init_0.mp4");
    }

    // Looks once a second up to second `last`, counted from the start; returns the second of
    // the first verdict that was not healthy, or -1 if there was none.
    int look_until(int last, int publisher_sends_until, Verdict& verdict) {
        verdict = Verdict::Healthy;
        while (elapsed_s < last) {
            ++elapsed_s;
            clock.advance(core::Millis{1000});
            verdict = watch->look(ffmpeg_list(elapsed_s, publisher_sends_until), clock.now());
            if (verdict != Verdict::Healthy) {
                return elapsed_s;
            }
        }
        return -1;
    }

    ulw::test::TempDir root{"ulw-live-watch"};
    ulw::test::FakeClock clock;
    ulw::test::RecordingStore store;
    std::optional<live::Publisher> publisher;
    std::optional<live::Watch> watch;
    int written = 0;
    int elapsed_s = 0;
};

TEST(WatchLimits, AreDerivedFromTheSegmentLengthAndTheListedSegments) {
    const auto defaults = live::watch_limits(2, live::listed_segments(10));
    EXPECT_EQ(defaults.upload_patience, std::chrono::seconds(20));
    EXPECT_EQ(defaults.stall, std::chrono::seconds(10));
    const auto widest = live::watch_limits(10, live::listed_segments(64));
    EXPECT_EQ(widest.upload_patience, std::chrono::seconds(640));
    EXPECT_EQ(widest.stall, std::chrono::seconds(50));
}

TEST_F(WatchTest, AHealthyPublisherAndStoreStayHealthy) {
    start();
    Verdict verdict{};
    EXPECT_EQ(look_until(60, 1000, verdict), -1);
    EXPECT_EQ(publisher->window().next_sequence(), 30U);
}

TEST_F(WatchTest, AStoreOutageShorterThanThePatienceDoesNotEndTheStream) {
    start();
    Verdict verdict{};
    ASSERT_EQ(look_until(4, 1000, verdict), -1);
    // 15 s without an upload succeeding: past the stall limit of 10 s, inside the patience of
    // 20 s. ffmpeg goes on listing segments the whole time.
    store.fail_all_uploads = true;
    EXPECT_EQ(look_until(19, 1000, verdict), -1) << static_cast<int>(verdict);
    store.fail_all_uploads = false;
    EXPECT_EQ(look_until(25, 1000, verdict), -1);
    // Everything ffmpeg listed during the outage went up once the store came back.
    EXPECT_GE(publisher->window().next_sequence(), 12U);
}

TEST_F(WatchTest, AStoreOutageLongerThanThePatienceGivesUpAndSaysItWasTheStore) {
    start();
    Verdict verdict{};
    ASSERT_EQ(look_until(4, 1000, verdict), -1);
    store.fail_all_uploads = true;
    // Nothing is left to upload until ffmpeg lists the next segment, at second 6: the first
    // failure, from which the patience of 20 s runs.
    const int second = look_until(60, 1000, verdict);
    EXPECT_EQ(verdict, Verdict::StoreGivenUp);
    EXPECT_EQ(second, 6 + 20);
    EXPECT_EQ(watch->last_error(), live::PublishError::UploadFailed);
}

TEST_F(WatchTest, AnOutageThatEndsJustBeforeThePatienceRunsOutStartsItOver) {
    start();
    Verdict verdict{};
    ASSERT_EQ(look_until(4, 1000, verdict), -1);
    store.fail_all_uploads = true;
    ASSERT_EQ(look_until(19, 1000, verdict), -1);
    store.fail_all_uploads = false;
    ASSERT_EQ(look_until(22, 1000, verdict), -1);
    store.fail_all_uploads = true;
    // 17 s of failures, and 36 s since the first outage began: it is the failing that is timed,
    // and success in between started it over.
    EXPECT_EQ(look_until(41, 1000, verdict), -1);
}

TEST_F(WatchTest, APublisherThatSendsNothingIsStalledAfterFiveSegmentLengths) {
    start();
    Verdict verdict{};
    EXPECT_EQ(look_until(60, 0, verdict), 10);
    EXPECT_EQ(verdict, Verdict::Stalled);
    EXPECT_FALSE(watch->last_error());
}

TEST_F(WatchTest, AnFfmpegThatStopsFinishingSegmentsIsStalledWhateverTheStoreDoes) {
    start();
    Verdict verdict{};
    // Segments until second 12, the last listed then; ten quiet seconds after it.
    const int second = look_until(60, 12, verdict);
    EXPECT_EQ(verdict, Verdict::Stalled);
    EXPECT_EQ(second, 22);
}

TEST_F(WatchTest, AStalledPublisherAndAFailingStoreAreStalledNotBlamedOnTheStore) {
    start();
    Verdict verdict{};
    ASSERT_EQ(look_until(5, 1000, verdict), -1);
    store.fail_all_uploads = true;
    const int second = look_until(60, 6, verdict);
    EXPECT_EQ(verdict, Verdict::Stalled);
    EXPECT_EQ(second, 16);
}

TEST_F(WatchTest, APlaylistThatDisagreesWithItselfIsBroken) {
    start();
    Verdict verdict{};
    ASSERT_EQ(look_until(4, 1000, verdict), -1);
    std::string text = ffmpeg_playlist(0, 0, 3, "init_0.mp4");
    text.replace(text.find("seg_0_2"), 7, "seg_0_9");
    EXPECT_EQ(watch->look(text, clock.now()), Verdict::Broken);
    EXPECT_EQ(watch->last_error(), live::PublishError::PlaylistInconsistent);
}

TEST_F(WatchTest, ASegmentPastTheContractIsBroken) {
    start();
    ulw::test::write_file(media() / "seg_0_0.m4s");
    const std::string text = ffmpeg_playlist(0, 0, 1, "init_0.mp4", 4.0);
    EXPECT_EQ(watch->look(text, clock.now()), Verdict::Broken);
    EXPECT_EQ(watch->last_error(), live::PublishError::KeyframeIntervalExceeded);
}

TEST_F(WatchTest, ANewerPackagerMakesThisOneSuperseded) {
    start();
    Verdict verdict{};
    ASSERT_EQ(look_until(4, 1000, verdict), -1);
    auto newer =
        live::Publisher::open({.stream = *live::StreamId::parse("show"),
                               .window = {.target_seconds = kSegment, .max_segments = kWindow},
                               .media_dir = root.path() / "media2",
                               .outbox = root.path() / "outbox2"},
                              store, clock);
    ASSERT_TRUE(newer);
    // Found at the next playlist write, when ffmpeg lists the next segment.
    EXPECT_EQ(look_until(10, 1000, verdict), 6);
    EXPECT_EQ(verdict, Verdict::Superseded);
}

} // namespace
