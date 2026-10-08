#include "core/models/content_type.hpp"

#include "reaper.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"

#include <algorithm>
#include <deque>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <vector>

namespace {

using core::ports::CatalogError;
using core::ports::ExpiredUpload;
using core::ports::StorageError;

class FakeCatalog final : public core::ports::IUploadExpiry {
public:
    [[nodiscard]] std::expected<std::vector<ExpiredUpload>, CatalogError>
    expire(core::WallTime now, std::size_t limit) override {
        calls.push_back({now, limit});
        if (replies.empty()) {
            return std::vector<ExpiredUpload>{};
        }
        auto reply = std::move(replies.front());
        replies.pop_front();
        return reply;
    }

    struct Call {
        core::WallTime now;
        std::size_t limit;
    };
    std::vector<Call> calls;
    std::deque<std::expected<std::vector<ExpiredUpload>, CatalogError>> replies;
};

class FakeRooms final : public core::ports::IUnusedRooms {
public:
    [[nodiscard]] std::expected<core::ports::UnusedRoomsScan, CatalogError>
    forget_unused(core::WallTime recorded_before, std::size_t limit) override {
        cutoffs.push_back(recorded_before);
        limits.push_back(limit);
        if (replies.empty()) {
            return core::ports::UnusedRoomsScan{.forgotten = 0, .finished = true};
        }
        const auto reply = replies.front();
        replies.pop_front();
        return reply;
    }

    std::vector<core::WallTime> cutoffs;
    std::vector<std::size_t> limits;
    std::deque<std::expected<core::ports::UnusedRoomsScan, CatalogError>> replies;
};

// video_purges as the reaper sees it: what due() answers, and what forget() was asked.
class FakePurges final : public core::ports::IVideoPurges {
public:
    [[nodiscard]] std::expected<std::vector<core::VideoId>, CatalogError>
    due(std::size_t limit) override {
        limits.push_back(limit);
        if (due_error) {
            return std::unexpected(*due_error);
        }
        std::vector<core::VideoId> out;
        for (const core::VideoId& v : queue) {
            if (out.size() == limit) {
                break;
            }
            out.push_back(v);
        }
        return out;
    }
    [[nodiscard]] std::expected<bool, CatalogError> forget(const core::VideoId& video) override {
        forgotten.push_back(video);
        if (forget_answer && *forget_answer) {
            std::erase(queue, video);
        }
        return forget_answer;
    }

    std::vector<core::VideoId> queue;
    std::optional<CatalogError> due_error;
    std::expected<bool, CatalogError> forget_answer = true;
    std::vector<std::size_t> limits;
    std::vector<core::VideoId> forgotten;
};

class FakeStore final : public core::ports::IIngestStore, public core::ports::IObjectAdmin {
public:
    [[nodiscard]] std::expected<core::ports::IngestId, StorageError>
    create(const core::StorageKey& /*key*/, std::uint64_t /*total*/,
           const core::ContentType& /*type*/) override {
        return std::unexpected(StorageError::Permanent);
    }
    [[nodiscard]] std::expected<std::unique_ptr<core::ports::IIngestSession>, StorageError>
    open(const core::ports::IngestId& /*id*/, std::uint64_t /*offset*/,
         core::ports::IIngestObserver& /*observer*/) override {
        return std::unexpected(StorageError::Permanent);
    }
    [[nodiscard]] std::expected<std::uint64_t, StorageError>
    durable_offset(const core::ports::IngestId& /*id*/) override {
        return still_held ? std::expected<std::uint64_t, StorageError>(1)
                          : std::unexpected(released_answer);
    }
    [[nodiscard]] std::expected<void, StorageError>
    commit(const core::ports::IngestId& /*id*/) override {
        return std::unexpected(StorageError::Permanent);
    }
    void discard(const core::ports::IngestId& id) noexcept override {
        discarded.push_back(id.backend_ref);
    }
    [[nodiscard]] std::uint64_t preferred_chunk_size() const noexcept override { return 1; }

    [[nodiscard]] std::expected<void, StorageError>
    put(const core::StorageKey& /*key*/, std::span<const std::byte> /*bytes*/) override {
        return std::unexpected(StorageError::Permanent);
    }
    [[nodiscard]] std::expected<void, StorageError> remove(const core::StorageKey& key) override {
        removed.emplace_back(key.view());
        if (removal) {
            std::erase(objects, std::string(key.view()));
        }
        return removal;
    }
    [[nodiscard]] std::expected<std::vector<core::StorageKey>, StorageError>
    list(std::string_view prefix) override {
        listed.emplace_back(prefix);
        if (list_error) {
            return std::unexpected(*list_error);
        }
        std::vector<core::StorageKey> keys;
        for (const std::string& object : objects) {
            if (object.starts_with(prefix)) {
                keys.push_back(*core::StorageKey::parse(object));
            }
        }
        return keys;
    }
    [[nodiscard]] std::expected<std::size_t, StorageError>
    reap_abandoned(core::WallTime older_than) override {
        cutoffs.push_back(older_than);
        return sweep;
    }

    std::vector<std::string> discarded;
    std::vector<std::string> removed;
    // What list() finds, and the prefixes it was asked for.
    std::vector<std::string> objects;
    std::vector<std::string> listed;
    std::optional<StorageError> list_error;
    std::expected<void, StorageError> removal;
    bool still_held = false;
    StorageError released_answer = StorageError::NotFound;
    std::vector<core::WallTime> cutoffs;
    std::expected<std::size_t, StorageError> sweep = 0;
};

class ReaperTest : public ::testing::Test {
protected:
    [[nodiscard]] std::vector<ExpiredUpload> uploads(std::size_t n) {
        std::vector<ExpiredUpload> out;
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back({.id = core::UploadId::generate(clock, random),
                           .ingest = {.key = *core::StorageKey::parse("videos/v/original"),
                                      .backend_ref = "session-" + std::to_string(next_++),
                                      .total_bytes = 1,
                                      .chunk_size = 1}});
        }
        return out;
    }

    [[nodiscard]] reaper::Report run(std::size_t batch = 3, std::size_t rooms_per_pass = 10'000,
                                     std::size_t videos_per_pass = 1'000) {
        return reaper::run_once(catalog, store, store, rooms, purges, clock,
                                {.batch = batch,
                                 .orphan_after = std::chrono::hours(7 * 24),
                                 .unused_room_after = std::chrono::hours(24),
                                 .rooms_per_pass = rooms_per_pass,
                                 .videos_per_pass = videos_per_pass});
    }

    // A deleted video, queued for its purge, and its source and renditions in the store.
    core::VideoId deleted_video() {
        const core::VideoId video = core::VideoId::generate(clock, random);
        purges.queue.push_back(video);
        const std::string prefix = "videos/" + video.to_string() + "/";
        for (const char* key : {"raw", "hls/master.m3u8", "hls/720p/index.m3u8",
                                "hls/720p/init_0.mp4", "hls/720p/seg_00000.m4s"}) {
            store.objects.push_back(prefix + key);
        }
        return video;
    }

    ulw::test::FakeClock clock;
    ulw::test::FakeRandom random;
    FakeCatalog catalog;
    FakeStore store;
    FakeRooms rooms;
    FakePurges purges;

private:
    int next_ = 0;
};

TEST_F(ReaperTest, ReleasesTheSessionOfEveryUploadTheCatalogExpired) {
    catalog.replies.emplace_back(uploads(2));
    const auto report = run();
    EXPECT_EQ(report.uploads_expired, 2U);
    EXPECT_EQ(store.discarded, (std::vector<std::string>{"session-0", "session-1"}));
    EXPECT_TRUE(report.problems.empty());
}

TEST_F(ReaperTest, AlsoRemovesTheObjectACommitFinishedJustAheadOfTheAbort) {
    catalog.replies.emplace_back(uploads(2));
    static_cast<void>(run());
    EXPECT_EQ(store.removed, (std::vector<std::string>{"videos/v/original", "videos/v/original"}));
}

TEST_F(ReaperTest, NoObjectToRemoveIsTheUsualCase) {
    catalog.replies.emplace_back(uploads(1));
    store.removal = std::unexpected(StorageError::NotFound);
    const auto report = run();
    EXPECT_EQ(report.uploads_expired, 1U);
    EXPECT_TRUE(report.problems.empty());
}

TEST_F(ReaperTest, AnObjectThatCannotBeRemovedIsAFailureNotAnExpiry) {
    catalog.replies.emplace_back(uploads(1));
    store.removal = std::unexpected(StorageError::Transient);
    const auto report = run();
    EXPECT_EQ(report.uploads_expired, 0U);
    EXPECT_EQ(report.uploads_release_failed, 1U);
    ASSERT_EQ(report.problems.size(), 1U);
    EXPECT_NE(report.problems[0].find("videos/v/original"), std::string::npos);
}

TEST_F(ReaperTest, ASessionTheStoreStillHoldsIsAFailureNotAnExpiry) {
    catalog.replies.emplace_back(uploads(1));
    store.still_held = true;
    const auto report = run();
    EXPECT_EQ(report.uploads_expired, 0U);
    EXPECT_EQ(report.uploads_release_failed, 1U);
}

TEST_F(ReaperTest, AStoreThatCannotSayWhetherItLetGoIsAFailureToo) {
    catalog.replies.emplace_back(uploads(1));
    store.released_answer = StorageError::Transient;
    const auto report = run();
    EXPECT_EQ(report.uploads_expired, 0U);
    EXPECT_EQ(report.uploads_release_failed, 1U);
}

TEST_F(ReaperTest, AsksAgainOnlyWhileBatchesComeBackFull) {
    catalog.replies.emplace_back(uploads(3));
    catalog.replies.emplace_back(uploads(3));
    catalog.replies.emplace_back(uploads(1));
    const auto report = run(3);
    EXPECT_EQ(report.uploads_expired, 7U);
    EXPECT_EQ(catalog.calls.size(), 3U);
    for (const auto& call : catalog.calls) {
        EXPECT_EQ(call.limit, 3U);
    }
}

TEST_F(ReaperTest, ExpiresAsOfTheWallClock) {
    static_cast<void>(run());
    ASSERT_EQ(catalog.calls.size(), 1U);
    EXPECT_EQ(catalog.calls[0].now, clock.wall_now());
}

TEST_F(ReaperTest, SweepsOnlySessionsOlderThanAnyUploadMayBe) {
    static_cast<void>(run());
    ASSERT_EQ(store.cutoffs.size(), 1U);
    EXPECT_EQ(store.cutoffs[0], clock.wall_now() - std::chrono::hours(7 * 24));
}

TEST_F(ReaperTest, CountsTheSessionsTheSweepAborted) {
    store.sweep = 4;
    const auto report = run();
    EXPECT_EQ(report.parts_orphaned, 4U);
    EXPECT_EQ(reaper::metrics_text(report),
              "# TYPE reaper_uploads_expired_last_run gauge\nreaper_uploads_expired_last_run 0\n"
              "# TYPE reaper_uploads_release_failed_last_run gauge\n"
              "reaper_uploads_release_failed_last_run 0\n"
              "# TYPE reaper_parts_orphaned_last_run gauge\nreaper_parts_orphaned_last_run 4\n"
              "# TYPE reaper_chat_rooms_forgotten_last_run gauge\n"
              "reaper_chat_rooms_forgotten_last_run 0\n"
              "# TYPE reaper_videos_purged_last_run gauge\nreaper_videos_purged_last_run 0\n"
              "# TYPE reaper_videos_purge_failed_last_run gauge\n"
              "reaper_videos_purge_failed_last_run 0\n");
}

TEST_F(ReaperTest, ACatalogFailureIsReportedAndTheSweepStillRuns) {
    catalog.replies.emplace_back(std::unexpected(CatalogError::Unavailable));
    store.sweep = 2;
    const auto report = run();
    ASSERT_EQ(report.problems.size(), 1U);
    EXPECT_NE(report.problems[0].find("expire uploads"), std::string::npos);
    EXPECT_EQ(report.parts_orphaned, 2U);
    EXPECT_EQ(report.uploads_expired, 0U);
}

TEST_F(ReaperTest, AFailedSweepIsReportedAndTheExpiredUploadsStillCount) {
    catalog.replies.emplace_back(uploads(1));
    store.sweep = std::unexpected(StorageError::Transient);
    const auto report = run();
    ASSERT_EQ(report.problems.size(), 1U);
    EXPECT_NE(report.problems[0].find("sweep"), std::string::npos);
    EXPECT_EQ(report.uploads_expired, 1U);
    EXPECT_EQ(report.parts_orphaned, 0U);
}

TEST_F(ReaperTest, UploadsAbortedBeforeAFailureKeepTheirReleasedSessions) {
    catalog.replies.emplace_back(uploads(3));
    catalog.replies.emplace_back(std::unexpected(CatalogError::Unavailable));
    const auto report = run(3);
    EXPECT_EQ(report.uploads_expired, 3U);
    EXPECT_EQ(store.discarded.size(), 3U);
    EXPECT_EQ(report.problems.size(), 1U);
}

// A refused join of a room nobody recorded records it (ADR-0054); nothing else ever removes the
// row, so the reaper forgets those a day old that nothing used, a batch at a time, until the walk
// reaches the cutoff (ADR-0075).
TEST_F(ReaperTest, ForgetsChatRoomsADayOldThatNothingUsedInBatchesUntilTheWalkEnds) {
    rooms.replies = {core::ports::UnusedRoomsScan{.forgotten = 3, .finished = false},
                     core::ports::UnusedRoomsScan{.forgotten = 0, .finished = false},
                     core::ports::UnusedRoomsScan{.forgotten = 4, .finished = true}};
    const auto report = run();
    EXPECT_EQ(report.rooms_forgotten, 7U);
    ASSERT_EQ(rooms.cutoffs.size(), 3U);
    EXPECT_EQ(rooms.cutoffs.front(), clock.wall_now() - std::chrono::hours(24));
    EXPECT_EQ(rooms.limits.front(), 3U);
    EXPECT_TRUE(report.problems.empty());
    EXPECT_NE(reaper::metrics_text(report).find("reaper_chat_rooms_forgotten_last_run 7\n"),
              std::string::npos);
}

TEST_F(ReaperTest, AFailureToForgetRoomsIsReportedAndTheUploadsStillCount) {
    catalog.replies.emplace_back(uploads(1));
    rooms.replies = {std::unexpected(CatalogError::Unavailable)};
    const auto report = run();
    EXPECT_EQ(report.uploads_expired, 1U);
    EXPECT_EQ(report.rooms_forgotten, 0U);
    ASSERT_EQ(report.problems.size(), 1U);
    EXPECT_NE(report.problems.front().find("chat rooms"), std::string::npos);
}

// A pass looks at a bounded share of the rooms, however many there are: the walk goes on from
// there in the next pass.
TEST_F(ReaperTest, APassLooksAtNoMoreRoomsThanItsShare) {
    for (int i = 0; i < 10; ++i) {
        rooms.replies.emplace_back(core::ports::UnusedRoomsScan{.forgotten = 1, .finished = false});
    }
    const auto report = run(3, 9);
    EXPECT_EQ(rooms.limits.size(), 3U);
    EXPECT_EQ(report.rooms_forgotten, 3U);
    EXPECT_TRUE(report.problems.empty());
}

// Deleted videos (ADR-0100): their objects, then their rows.
TEST_F(ReaperTest, PurgesEveryObjectOfADeletedVideoThenForgetsIt) {
    const core::VideoId first = deleted_video();
    const core::VideoId second = deleted_video();
    store.objects.emplace_back("videos/01890a5d-ac96-774b-bcce-b302099a8057/raw");
    const auto report = run();
    EXPECT_EQ(report.videos_purged, 2U);
    EXPECT_EQ(report.videos_purge_failed, 0U);
    EXPECT_TRUE(report.problems.empty());
    EXPECT_EQ(store.listed, (std::vector<std::string>{"videos/" + first.to_string() + "/",
                                                      "videos/" + second.to_string() + "/"}));
    EXPECT_EQ(store.removed.size(), 10U);
    // Only another video's object is left.
    EXPECT_EQ(store.objects,
              std::vector<std::string>{"videos/01890a5d-ac96-774b-bcce-b302099a8057/raw"});
    EXPECT_EQ(purges.forgotten, (std::vector<core::VideoId>{first, second}));
    EXPECT_TRUE(purges.queue.empty());
}

TEST_F(ReaperTest, AVideoWhoseObjectsStayIsKeptForTheNextPass) {
    deleted_video();
    store.removal = std::unexpected(StorageError::Transient);
    const auto failed = run();
    EXPECT_EQ(failed.videos_purged, 0U);
    EXPECT_EQ(failed.videos_purge_failed, 1U);
    ASSERT_EQ(failed.problems.size(), 1U);
    EXPECT_NE(failed.problems.front().find("remove videos/"), std::string::npos);
    // The row stays, so the video comes due again.
    EXPECT_TRUE(purges.forgotten.empty());
    EXPECT_EQ(purges.queue.size(), 1U);

    store.removal = {};
    const auto retried = run();
    EXPECT_EQ(retried.videos_purged, 1U);
    EXPECT_TRUE(store.objects.empty());
    EXPECT_TRUE(purges.queue.empty());
}

TEST_F(ReaperTest, AnObjectGoneAlreadyIsNoFailure) {
    deleted_video();
    store.removal = std::unexpected(StorageError::NotFound);
    const auto report = run();
    EXPECT_EQ(report.videos_purged, 1U);
    EXPECT_TRUE(report.problems.empty());
}

TEST_F(ReaperTest, AStoreThatCannotListLeavesTheVideoQueued) {
    deleted_video();
    store.list_error = StorageError::Unauthorized;
    const auto report = run();
    EXPECT_EQ(report.videos_purge_failed, 1U);
    ASSERT_EQ(report.problems.size(), 1U);
    EXPECT_NE(report.problems.front().find("list videos/"), std::string::npos);
    EXPECT_TRUE(store.removed.empty());
    EXPECT_TRUE(purges.forgotten.empty());
}

TEST_F(ReaperTest, AVideoAJobWasPutBackForIsLeftForALaterPass) {
    deleted_video();
    purges.forget_answer = false;
    const auto report = run();
    // Its objects went; its row stays until the job is cancelled, and then they go again.
    EXPECT_TRUE(store.objects.empty());
    EXPECT_EQ(report.videos_purged, 0U);
    EXPECT_EQ(report.videos_purge_failed, 0U);
    EXPECT_TRUE(report.problems.empty());
    EXPECT_EQ(purges.limits.size(), 1U) << "a batch left behind is not asked for again";
}

TEST_F(ReaperTest, ACatalogFailureToListDeletedVideosIsReported) {
    deleted_video();
    purges.due_error = CatalogError::Unavailable;
    store.sweep = 1;
    const auto report = run();
    EXPECT_EQ(report.parts_orphaned, 1U);
    ASSERT_EQ(report.problems.size(), 1U);
    EXPECT_NE(report.problems.front().find("purge deleted videos"), std::string::npos);
    EXPECT_EQ(store.objects.size(), 5U);
}

TEST_F(ReaperTest, APassPurgesNoMoreVideosThanItsShare) {
    for (int i = 0; i < 10; ++i) {
        static_cast<void>(deleted_video());
    }
    const auto report = run(3, 10'000, 6);
    EXPECT_EQ(report.videos_purged, 6U);
    EXPECT_EQ(purges.limits, (std::vector<std::size_t>{3, 3}));
    EXPECT_EQ(purges.queue.size(), 4U);
}

} // namespace
