#include "core/models/ids.hpp"
#include "core/models/video.hpp"
#include "core/models/video_access.hpp"
#include "core/models/visibility.hpp"
#include "infra/postgres/upload_catalog.hpp"
#include "infra/postgres/upload_reaper.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "postgres_harness.hpp"
#include "support/reactor_harness.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The catalog's and the reaper's side of ADR-0100 on a real database: a video's deletion by its
// owner or the operator's backend, the backend's visibility and listing, and the purge that
// follows, with migration 0017's queue.
namespace {

using core::ports::CatalogError;
using core::ports::CatalogResult;
using core::ports::VideoCursor;
using core::ports::VideoPage;
using infra::postgres::CatalogConfig;
using infra::postgres::Params;
using infra::postgres::PgUploadCatalog;
using infra::postgres::PgUploadReaper;
using ulw::test::Reply;
using ulw::test::scalar;
using ulw::test::ScratchDatabase;

constexpr std::string_view kRoom = "0192f3c4-7a1b-7c2d-8e3f-0123456789ab";

core::UserId user(std::string_view name) {
    return *core::UserId::parse(name);
}

class VodControlTest : public ::testing::Test {
protected:
    void SetUp() override {
        ScratchDatabase::open(db);
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        auto r = net::make_reactor(net::ReactorKind::Epoll, clock, 4096);
        ASSERT_TRUE(r) << "reactor: " << std::strerror(r.error());
        reactor = std::move(*r);
        auto p = net::OffloadPool::create(*reactor, 1);
        ASSERT_TRUE(p);
        offload = std::move(*p);
        auto made =
            PgUploadCatalog::create(*reactor, *offload, CatalogConfig{.conninfo = db->conninfo()});
        ASSERT_TRUE(made) << made.error();
        catalog = std::move(*made);
        reaper = std::make_unique<PgUploadReaper>(db->conninfo());
    }

    void TearDown() override {
        reaper.reset();
        offload.reset();
        catalog.reset();
        reactor.reset();
        db.reset();
    }

    template <class T, class Start> CatalogResult<T> call(Start start) {
        Reply<T> reply;
        start(reply.callback());
        return ulw::test::wait(*reactor, reply);
    }

    // A video of `owner`'s in `state`, created `age_s` seconds ago.
    core::VideoId add_video(std::string_view state = "ready", std::string_view owner = "alice",
                            std::int64_t age_s = 0) {
        const auto id = core::VideoId::generate(clock, random);
        auto conn = db->session();
        EXPECT_TRUE(
            conn.exec("INSERT INTO videos (id, owner_id, title, state, version, "
                      "duration_ms, error_reason, created_at) "
                      "VALUES ($1, $2, 'trip', $3::text::video_state, 2, "
                      "CASE WHEN $3 = 'ready' THEN 5000 END, "
                      "CASE WHEN $3 = 'failed' THEN 'bad' END, "
                      "now() - $4 * interval '1 second')",
                      Params{}.add_uuid(id.uuid()).add_text(owner).add_text(state).add_int(age_s)));
        return id;
    }

    void add_job(const core::VideoId& id, std::string_view state) {
        auto conn = db->session();
        ASSERT_TRUE(conn.exec("INSERT INTO jobs (video_id, kind, state, locked_by, lease_expires) "
                              "VALUES ($1, 'transcode', $2, "
                              "CASE WHEN $2 = 'running' THEN 'worker-1' END, "
                              "CASE WHEN $2 = 'running' THEN now() + interval '1 minute' END)",
                              Params{}.add_uuid(id.uuid()).add_text(state)));
    }

    std::string count(infra::postgres::Sql sql, const core::VideoId& id) {
        auto conn = db->session();
        return scalar(conn, sql, Params{}.add_uuid(id.uuid()));
    }

    CatalogResult<void> remove(const core::VideoId& id, std::optional<std::string_view> owner) {
        std::optional<core::UserId> who;
        if (owner) {
            who = user(*owner);
        }
        return call<void>([&](auto done) { catalog->delete_video(id, who, std::move(done)); });
    }
    CatalogResult<core::ports::VideoView> view(const core::VideoId& id, std::string_view viewer) {
        return call<core::ports::VideoView>(
            [&](auto done) { catalog->find_video_for(id, user(viewer), std::move(done)); });
    }
    CatalogResult<void> grant(const core::VideoId& id, std::string_view who) {
        return call<void>(
            [&](auto done) { catalog->grant_access(id, user(who), std::move(done)); });
    }
    CatalogResult<core::VideoRecord> set_by_service(const core::VideoId& id,
                                                    const core::Visibility& visibility) {
        return call<core::VideoRecord>([&](auto done) {
            catalog->set_visibility(id, std::nullopt, visibility, std::move(done));
        });
    }
    CatalogResult<VideoPage> list(std::string_view owner, std::optional<VideoCursor> after,
                                  std::size_t limit) {
        return call<VideoPage>(
            [&](auto done) { catalog->list_videos(user(owner), after, limit, std::move(done)); });
    }

    os::SystemClock clock;
    os::SystemRandom random;
    std::unique_ptr<ScratchDatabase> db;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<net::OffloadPool> offload;
    std::unique_ptr<PgUploadCatalog> catalog;
    std::unique_ptr<PgUploadReaper> reaper;
};

TEST_F(VodControlTest, TheOwnerDeletesACommittedVideoAndEveryReadLosesIt) {
    for (const std::string_view state : {"processing", "ready", "failed"}) {
        const auto id = add_video(state);
        ASSERT_TRUE(grant(id, "bob"));
        const auto deleted = remove(id, "alice");
        ASSERT_TRUE(deleted) << state << ": " << core::ports::to_string(deleted.error());
        EXPECT_EQ(view(id, "alice").error(), CatalogError::NotFound) << state;
        EXPECT_EQ(view(id, "bob").error(), CatalogError::NotFound) << state;
        EXPECT_EQ(grant(id, "carol").error(), CatalogError::NotFound);
        EXPECT_EQ(count("SELECT count(*) FROM video_grants WHERE video_id = $1", id), "0");
        EXPECT_EQ(count("SELECT count(*) FROM video_purges WHERE video_id = $1", id), "1");
        // The row stays for the reaper, marked.
        EXPECT_EQ(count("SELECT count(*) FROM videos WHERE id = $1 AND deleted_at IS NOT NULL", id),
                  "1");
    }
}

TEST_F(VodControlTest, DeletingAgainSucceedsAndChangesNothing) {
    const auto id = add_video();
    ASSERT_TRUE(remove(id, "alice"));
    const std::string at = count("SELECT deleted_at::text FROM videos WHERE id = $1", id);
    EXPECT_TRUE(remove(id, "alice"));
    EXPECT_TRUE(remove(id, std::nullopt));
    EXPECT_EQ(count("SELECT deleted_at::text FROM videos WHERE id = $1", id), at);
    EXPECT_EQ(count("SELECT count(*) FROM video_purges WHERE video_id = $1", id), "1");
}

TEST_F(VodControlTest, OnlyTheOwnerOrTheServiceDeletes) {
    const auto id = add_video();
    EXPECT_EQ(remove(id, "bob").error(), CatalogError::NotFound);
    EXPECT_TRUE(view(id, "alice"));
    EXPECT_EQ(remove(core::VideoId::generate(clock, random), std::nullopt).error(),
              CatalogError::NotFound);
    // The service may delete anyone's: a takedown.
    EXPECT_TRUE(remove(id, std::nullopt));
    EXPECT_EQ(view(id, "alice").error(), CatalogError::NotFound);
}

TEST_F(VodControlTest, AnUploadInProgressIsNotDeleted) {
    for (const std::string_view state : {"init", "uploading"}) {
        const auto id = add_video(state);
        EXPECT_EQ(remove(id, "alice").error(), CatalogError::Conflict) << state;
        EXPECT_EQ(remove(id, std::nullopt).error(), CatalogError::Conflict) << state;
        EXPECT_TRUE(view(id, "alice")) << state;
        EXPECT_EQ(count("SELECT count(*) FROM video_purges WHERE video_id = $1", id), "0");
        // Someone else's still answers as missing.
        EXPECT_EQ(remove(id, "bob").error(), CatalogError::NotFound);
    }
}

TEST_F(VodControlTest, ADeletionCancelsAQueuedJobAndLeavesARunningOne) {
    const auto queued = add_video("processing");
    add_job(queued, "queued");
    ASSERT_TRUE(remove(queued, "alice"));
    EXPECT_EQ(count("SELECT state || ':' || last_error FROM jobs WHERE video_id = $1", queued),
              "failed:video deleted");

    const auto running = add_video("processing");
    add_job(running, "running");
    ASSERT_TRUE(remove(running, "alice"));
    EXPECT_EQ(count("SELECT state FROM jobs WHERE video_id = $1", running), "running");
}

TEST_F(VodControlTest, TheServiceSetsAnyVideosVisibilityToARoomItsOwnerIsIn) {
    const auto id = add_video();
    const auto unlisted = set_by_service(id, core::Visibility::unlisted());
    ASSERT_TRUE(unlisted) << core::ports::to_string(unlisted.error());
    EXPECT_EQ(unlisted->visibility, core::Visibility::unlisted());
    const auto room = core::Visibility::room(*core::RoomId::parse(kRoom));
    EXPECT_EQ(set_by_service(id, room).error(), CatalogError::Forbidden);
    auto conn = db->session();
    ASSERT_TRUE(conn.exec("INSERT INTO chat_rooms (room_id, kind) VALUES ($1, 'group_chat')",
                          Params{}.add_uuid(core::RoomId::parse(kRoom)->uuid())));
    // Listing someone else in the room is not enough: the owner must be.
    ASSERT_TRUE(conn.exec("INSERT INTO chat_members (room_id, user_id) VALUES ($1, 'bob')",
                          Params{}.add_uuid(core::RoomId::parse(kRoom)->uuid())));
    EXPECT_EQ(set_by_service(id, room).error(), CatalogError::Forbidden);
    ASSERT_TRUE(conn.exec("INSERT INTO chat_members (room_id, user_id) VALUES ($1, 'alice')",
                          Params{}.add_uuid(core::RoomId::parse(kRoom)->uuid())));
    const auto shared = set_by_service(id, room);
    ASSERT_TRUE(shared) << core::ports::to_string(shared.error());
    EXPECT_EQ(shared->visibility, room);
    EXPECT_EQ(core::access_of(view(id, "bob")->video, user("bob"), view(id, "bob")->viewer),
              core::VideoAccess::Viewer);
    EXPECT_EQ(set_by_service(core::VideoId::generate(clock, random), {}).error(),
              CatalogError::NotFound);
    ASSERT_TRUE(remove(id, std::nullopt));
    EXPECT_EQ(set_by_service(id, {}).error(), CatalogError::NotFound);
}

TEST_F(VodControlTest, AUsersVideosAreListedNewestFirstAPageAtATime) {
    const auto oldest = add_video("failed", "alice", 30);
    const auto middle = add_video("init", "alice", 20);
    const auto newest = add_video("ready", "alice", 10);
    const auto gone = add_video("ready", "alice", 5);
    static_cast<void>(add_video("ready", "bob", 1));
    ASSERT_TRUE(remove(gone, "alice"));

    const auto all = list("alice", std::nullopt, 10);
    ASSERT_TRUE(all) << core::ports::to_string(all.error());
    ASSERT_EQ(all->videos.size(), 3U);
    EXPECT_FALSE(all->more);
    EXPECT_EQ(all->videos[0].video.id, newest);
    EXPECT_EQ(all->videos[1].video.id, middle);
    EXPECT_EQ(all->videos[2].video.id, oldest);
    EXPECT_EQ(all->videos[1].video.state, core::VideoState::Init);
    EXPECT_EQ(all->videos[2].video.error_reason, "bad");
    EXPECT_GT(all->videos[0].created_at_us, all->videos[1].created_at_us);

    const auto first = list("alice", std::nullopt, 2);
    ASSERT_TRUE(first);
    ASSERT_EQ(first->videos.size(), 2U);
    EXPECT_TRUE(first->more);
    const auto& last = first->videos.back();
    const auto rest =
        list("alice", VideoCursor{.created_at_us = last.created_at_us, .id = last.video.id}, 2);
    ASSERT_TRUE(rest) << core::ports::to_string(rest.error());
    ASSERT_EQ(rest->videos.size(), 1U);
    EXPECT_EQ(rest->videos[0].video.id, oldest);
    EXPECT_FALSE(rest->more);

    const auto nobody = list("carol", std::nullopt, 10);
    ASSERT_TRUE(nobody);
    EXPECT_TRUE(nobody->videos.empty());
}

TEST_F(VodControlTest, VideosCreatedAtTheSameInstantAreOrderedByIdAcrossPages) {
    auto conn = db->session();
    std::vector<core::VideoId> ids;
    ids.reserve(5);
    for (int i = 0; i < 5; ++i) {
        ids.push_back(add_video());
    }
    ASSERT_TRUE(
        conn.exec("UPDATE videos SET created_at = timestamptz '2026-10-08 12:00:00.123456'"));
    std::vector<core::VideoId> seen;
    std::optional<VideoCursor> after;
    for (int page = 0; page < 5; ++page) {
        const auto got = list("alice", after, 2);
        ASSERT_TRUE(got) << core::ports::to_string(got.error());
        for (const auto& v : got->videos) {
            seen.push_back(v.video.id);
        }
        if (!got->more) {
            break;
        }
        after = VideoCursor{.created_at_us = got->videos.back().created_at_us,
                            .id = got->videos.back().video.id};
    }
    std::ranges::sort(ids, [](const auto& a, const auto& b) { return b < a; });
    EXPECT_EQ(seen, ids);
}

TEST_F(VodControlTest, TheReaperPurgesADeletedVideoOnceNoJobRunsForIt) {
    const auto id = add_video("processing");
    add_job(id, "running");
    ASSERT_TRUE(grant(id, "bob"));
    ASSERT_TRUE(remove(id, "alice"));
    // A worker still transcodes it: not due, so no rendition is written after the purge.
    const auto busy = reaper->due(10);
    ASSERT_TRUE(busy);
    EXPECT_TRUE(busy->empty());

    // The worker finishes; the video is due, and forgetting it takes its row and its queue entry.
    auto conn = db->session();
    ASSERT_TRUE(conn.exec("UPDATE jobs SET state = 'done' WHERE video_id = $1",
                          Params{}.add_uuid(id.uuid())));
    const auto due = reaper->due(10);
    ASSERT_TRUE(due);
    ASSERT_EQ(due->size(), 1U);
    EXPECT_EQ(due->front(), id);
    const auto forgotten = reaper->forget(id);
    ASSERT_TRUE(forgotten);
    EXPECT_TRUE(*forgotten);
    EXPECT_EQ(count("SELECT count(*) FROM videos WHERE id = $1", id), "0");
    EXPECT_EQ(count("SELECT count(*) FROM jobs WHERE video_id = $1", id), "0");
    EXPECT_EQ(count("SELECT count(*) FROM video_purges WHERE video_id = $1", id), "0");
    EXPECT_TRUE(reaper->due(10)->empty());
    // Gone for good: a later delete is told there is no such video.
    EXPECT_EQ(remove(id, "alice").error(), CatalogError::NotFound);
}

TEST_F(VodControlTest, AJobPutBackAfterTheDeletionIsCancelledBeforeThePurge) {
    const auto id = add_video("processing");
    add_job(id, "running");
    ASSERT_TRUE(remove(id, "alice"));
    // The worker's lease lapsed and the job was put back (the job reaper's requeue).
    auto conn = db->session();
    ASSERT_TRUE(conn.exec("UPDATE jobs SET state = 'queued', locked_by = NULL, "
                          "lease_expires = NULL WHERE video_id = $1",
                          Params{}.add_uuid(id.uuid())));
    // A forget meanwhile leaves the row: the job could still write renditions.
    EXPECT_EQ(reaper->forget(id), false);
    EXPECT_EQ(count("SELECT count(*) FROM videos WHERE id = $1", id), "1");
    // The first look cancels it; the next finds the video due.
    EXPECT_TRUE(reaper->due(10)->empty());
    EXPECT_EQ(count("SELECT state FROM jobs WHERE video_id = $1", id), "failed");
    const auto due = reaper->due(10);
    ASSERT_TRUE(due);
    ASSERT_EQ(due->size(), 1U);
    EXPECT_EQ(reaper->forget(id), true);
}

TEST_F(VodControlTest, TheReaperTakesTheLongestDeletedFirstAndSkipsARestoredVideo) {
    const auto first = add_video();
    const auto second = add_video();
    const auto restored = add_video();
    for (const auto& id : {first, second, restored}) {
        ASSERT_TRUE(remove(id, std::nullopt));
    }
    auto conn = db->session();
    ASSERT_TRUE(conn.exec("UPDATE video_purges SET deleted_at = now() - interval '1 hour' "
                          "WHERE video_id = $1",
                          Params{}.add_uuid(second.uuid())));
    // An operator undid this one's deletion.
    ASSERT_TRUE(conn.exec("UPDATE videos SET deleted_at = NULL WHERE id = $1",
                          Params{}.add_uuid(restored.uuid())));
    // Its transcode, queued again, is its own: the reaper leaves it alone.
    add_job(restored, "queued");
    const auto due = reaper->due(10);
    EXPECT_EQ(count("SELECT state FROM jobs WHERE video_id = $1", restored), "queued");
    ASSERT_TRUE(due);
    ASSERT_EQ(due->size(), 2U);
    EXPECT_EQ((*due)[0], second);
    EXPECT_EQ((*due)[1], first);
    EXPECT_EQ(reaper->due(1)->size(), 1U);
    // A restored video is never forgotten: its row stays.
    EXPECT_EQ(reaper->forget(restored), false);
    EXPECT_TRUE(view(restored, "alice"));
}

TEST_F(VodControlTest, AQueueEntryWhoseRowIsGoneIsDropped) {
    const auto id = add_video();
    ASSERT_TRUE(remove(id, "alice"));
    auto conn = db->session();
    ASSERT_TRUE(conn.exec("DELETE FROM videos WHERE id = $1", Params{}.add_uuid(id.uuid())));
    const auto due = reaper->due(10);
    ASSERT_TRUE(due);
    ASSERT_EQ(due->size(), 1U);
    EXPECT_EQ(reaper->forget(id), true);
    EXPECT_EQ(count("SELECT count(*) FROM video_purges WHERE video_id = $1", id), "0");
}

} // namespace
