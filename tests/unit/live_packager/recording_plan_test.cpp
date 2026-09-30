#include "recording_plan.hpp"
#include "support.hpp"
#include "support/temp_dir.hpp"

#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using live::PlanError;
using live::RecordingRun;

class RecordingPlanTest : public ::testing::Test {
protected:
    RecordingPlanTest() { fs::create_directories(dir()); }

    [[nodiscard]] fs::path dir() const { return root.path() / "objects/live/show"; }

    // Segments first..last of `epoch` in the store.
    void stored(std::uint32_t epoch, std::uint64_t first, std::uint64_t last) const {
        for (std::uint64_t n = first; n <= last; ++n) {
            ulw::test::write_file(
                dir() / ("seg_" + std::to_string(epoch) + "_" + std::to_string(n) + ".m4s"));
        }
    }

    // An ended playlist listing `epochs.size()` segments from `media_sequence`, each of the
    // epoch given for it.
    static live::MediaPlaylist ended(std::uint64_t media_sequence,
                                     const std::vector<std::uint32_t>& epochs) {
        live::MediaPlaylist playlist{.target_seconds = 2,
                                     .media_sequence = media_sequence,
                                     .discontinuity_sequence = 0,
                                     .ended = true,
                                     .segments = {}};
        for (std::size_t i = 0; i < epochs.size(); ++i) {
            playlist.segments.push_back({.uri = "seg_" + std::to_string(epochs[i]) + "_" +
                                                std::to_string(media_sequence + i) + ".m4s",
                                         .duration = live::Micros{2'000'000},
                                         .init = "init_" + std::to_string(epochs[i]) + ".mp4",
                                         .discontinuity = false,
                                         .program_date_time = std::nullopt});
        }
        return playlist;
    }

    std::expected<live::RecordingPlan, PlanError> plan(const live::MediaPlaylist& playlist) {
        return live::plan_recording(playlist, stream, store);
    }

    ulw::test::TempDir root{"ulw-recording-plan"};
    ulw::test::RecordingStore store{root.path()};
    live::StreamId stream = *live::StreamId::parse("show");
};

TEST_F(RecordingPlanTest, OneRunIsTakenWholeIncludingWhatSlidOutOfTheWindow) {
    stored(0, 0, 9);
    const auto got = plan(ended(6, {0, 0, 0, 0}));
    ASSERT_TRUE(got);
    EXPECT_EQ(got->runs, (std::vector<RecordingRun>{{.epoch = 0, .first = 0, .last = 9}}));
    EXPECT_EQ(got->missing, 0U);
}

TEST_F(RecordingPlanTest, ARestartedRunFollowsTheOneBeforeIt) {
    stored(0, 0, 5);
    stored(1, 6, 12);
    const auto got = plan(ended(9, {1, 1, 1, 1}));
    ASSERT_TRUE(got);
    EXPECT_EQ(got->runs, (std::vector<RecordingRun>{{.epoch = 0, .first = 0, .last = 5},
                                                    {.epoch = 1, .first = 6, .last = 12}}));
}

TEST_F(RecordingPlanTest, TheWindowNamesItsOwnersAcrossARestart) {
    stored(0, 0, 5);
    stored(1, 6, 8);
    const auto got = plan(ended(4, {0, 0, 1, 1, 1}));
    ASSERT_TRUE(got);
    EXPECT_EQ(got->runs, (std::vector<RecordingRun>{{.epoch = 0, .first = 0, .last = 5},
                                                    {.epoch = 1, .first = 6, .last = 8}}));
}

TEST_F(RecordingPlanTest, WhatASupersededRunWroteAfterItsSuccessorStartedIsLeftOut) {
    // Epoch 0 went on cutting 6 and 7 before it saw epoch 1's claim; epoch 1 published them.
    stored(0, 0, 7);
    stored(1, 6, 12);
    const auto got = plan(ended(12, {1}));
    ASSERT_TRUE(got);
    EXPECT_EQ(got->runs, (std::vector<RecordingRun>{{.epoch = 0, .first = 0, .last = 5},
                                                    {.epoch = 1, .first = 6, .last = 12}}));
}

TEST_F(RecordingPlanTest, AnEpochThatNeverPublishedIsSteppedOver) {
    stored(0, 0, 5);
    stored(2, 6, 12);
    const auto got = plan(ended(10, {2, 2, 2}));
    ASSERT_TRUE(got);
    EXPECT_EQ(got->runs, (std::vector<RecordingRun>{{.epoch = 0, .first = 0, .last = 5},
                                                    {.epoch = 2, .first = 6, .last = 12}}));
}

TEST_F(RecordingPlanTest, AHoleIsCountedAndTheRecordingGoesOnAroundIt) {
    stored(0, 0, 2);
    stored(0, 4, 9);
    const auto got = plan(ended(8, {0, 0}));
    ASSERT_TRUE(got);
    EXPECT_EQ(got->runs, (std::vector<RecordingRun>{{.epoch = 0, .first = 0, .last = 2},
                                                    {.epoch = 0, .first = 4, .last = 9}}));
    EXPECT_EQ(got->missing, 1U);
}

TEST_F(RecordingPlanTest, AStreamStillLiveOrNeverStartedHasNothingToRecord) {
    auto live_playlist = ended(0, {0});
    live_playlist.ended = false;
    EXPECT_EQ(plan(live_playlist).error(), PlanError::NotEnded);
    EXPECT_EQ(plan(ended(0, {})).error(), PlanError::Empty);
}

TEST_F(RecordingPlanTest, AStoreThatCannotAnswerIsNotTakenForAMissingSegment) {
    stored(0, 0, 9);
    store.size_error = core::ports::StorageError::Transient;
    EXPECT_EQ(plan(ended(8, {0, 0})).error(), PlanError::StoreUnreadable);
}

} // namespace
