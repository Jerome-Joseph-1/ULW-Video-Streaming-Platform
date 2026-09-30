// The reaper's statement against a real Postgres, next to the catalog it races: what a
// concurrent claim, commit or abort leaves it, which no fake can say.
#include "core/models/upload.hpp"
#include "core/models/video.hpp"
#include "infra/postgres/upload_catalog.hpp"
#include "infra/postgres/upload_reaper.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "postgres_harness.hpp"
#include "support/reactor_harness.hpp"

#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <string>

namespace {

using core::ports::CatalogError;
using core::ports::NewUpload;
using infra::postgres::CatalogConfig;
using infra::postgres::Params;
using infra::postgres::PgUploadCatalog;
using infra::postgres::PgUploadReaper;
using ulw::test::Reply;
using ulw::test::scalar;
using ulw::test::ScratchDatabase;

constexpr std::uint64_t kChunk = 8ULL << 20U;
constexpr std::chrono::hours kTtl{24};

class UploadReaperTest : public ::testing::Test {
protected:
    void SetUp() override {
        ScratchDatabase::open(db);
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        auto r = net::make_reactor(net::ReactorKind::Epoll, clock, 4096);
        ASSERT_TRUE(r);
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

    [[nodiscard]] NewUpload created() {
        const auto video = core::VideoId::generate(clock, random);
        const auto owner = *core::UserId::parse("auth0|tester");
        const NewUpload u{
            .video = core::VideoRecord{.id = video,
                                       .owner = owner,
                                       .title = "holiday",
                                       .state = core::VideoState::Init,
                                       .version = 0,
                                       .error_reason = std::nullopt,
                                       .duration = std::nullopt},
            .upload =
                core::UploadRecord{.id = core::UploadId::generate(clock, random),
                                   .video_id = video,
                                   .owner = owner,
                                   .size_bytes = 3 * kChunk,
                                   .chunk_size = kChunk,
                                   .durable_offset = 0,
                                   .state = core::UploadState::Active,
                                   .expires_at = std::chrono::floor<std::chrono::microseconds>(
                                       clock.wall_now() + kTtl)},
            .backend_ref = "session-" + video.to_string(),
            .object_key = *core::StorageKey::parse("videos/" + video.to_string() + "/raw")};
        Reply<void> reply;
        catalog->create_upload(u, reply.callback());
        EXPECT_TRUE(ulw::test::wait(*reactor, reply));
        return u;
    }

    template <class Start> core::ports::CatalogResult<void> call(Start start) {
        Reply<void> reply;
        start(reply.callback());
        return ulw::test::wait(*reactor, reply);
    }

    [[nodiscard]] std::string upload_state(const NewUpload& u) const {
        auto conn = db->session();
        return scalar(conn, "SELECT state FROM uploads WHERE id = $1",
                      Params{}.add_uuid(u.upload.id.uuid()));
    }
    [[nodiscard]] std::string video_state(const NewUpload& u) const {
        auto conn = db->session();
        return scalar(conn, "SELECT state FROM videos WHERE id = $1",
                      Params{}.add_uuid(u.video.id.uuid()));
    }
    [[nodiscard]] std::string video_reason(const NewUpload& u) const {
        auto conn = db->session();
        return scalar(conn, "SELECT coalesce(error_reason, '') FROM videos WHERE id = $1",
                      Params{}.add_uuid(u.video.id.uuid()));
    }
    [[nodiscard]] std::string video_version(const NewUpload& u) const {
        auto conn = db->session();
        return scalar(conn, "SELECT version FROM videos WHERE id = $1",
                      Params{}.add_uuid(u.video.id.uuid()));
    }

    [[nodiscard]] core::WallTime after_ttl() const {
        return clock.wall_now() + kTtl + std::chrono::minutes(1);
    }

    os::SystemClock clock;
    os::SystemRandom random;
    std::unique_ptr<ScratchDatabase> db;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<net::OffloadPool> offload;
    std::unique_ptr<PgUploadCatalog> catalog;
    std::unique_ptr<PgUploadReaper> reaper;
};

TEST_F(UploadReaperTest, AbortsAnExpiredUploadAndFailsItsVideoInOneStep) {
    const NewUpload u = created();
    const auto expired = reaper->expire(after_ttl(), 10);
    ASSERT_TRUE(expired);
    ASSERT_EQ(expired->size(), 1U);
    EXPECT_EQ(expired->front().id, u.upload.id);
    // What the store needs to release the session, exactly as it was persisted.
    EXPECT_EQ(expired->front().ingest.key, u.object_key);
    EXPECT_EQ(expired->front().ingest.backend_ref, u.backend_ref);
    EXPECT_EQ(expired->front().ingest.total_bytes, 3 * kChunk);
    EXPECT_EQ(expired->front().ingest.chunk_size, kChunk);
    EXPECT_EQ(upload_state(u), "aborted");
    EXPECT_EQ(video_state(u), "failed");
    EXPECT_EQ(video_reason(u), "upload expired");
    EXPECT_EQ(video_version(u), "1");
}

TEST_F(UploadReaperTest, LeavesUploadsWhoseTimeHasNotCome) {
    const NewUpload u = created();
    const auto expired = reaper->expire(clock.wall_now() + kTtl - std::chrono::minutes(1), 10);
    ASSERT_TRUE(expired);
    EXPECT_TRUE(expired->empty());
    EXPECT_EQ(upload_state(u), "active");
    EXPECT_EQ(video_state(u), "init");
}

TEST_F(UploadReaperTest, TakesAtMostTheLimitAndTheEarliestExpiryFirst) {
    const NewUpload first = created();
    const NewUpload second = created();
    const NewUpload third = created();
    const auto batch = reaper->expire(after_ttl(), 2);
    ASSERT_TRUE(batch);
    EXPECT_EQ(batch->size(), 2U);
    EXPECT_EQ(upload_state(first), "aborted");
    EXPECT_EQ(upload_state(second), "aborted");
    EXPECT_EQ(upload_state(third), "active");
    const auto rest = reaper->expire(after_ttl(), 2);
    ASSERT_TRUE(rest);
    EXPECT_EQ(rest->size(), 1U);
}

TEST_F(UploadReaperTest, IgnoresUploadsThatCompletedOrWereAbortedByTheirOwner) {
    const NewUpload committed = created();
    const NewUpload discarded = created();
    Reply<core::ports::StoredUpload> claimed;
    catalog->claim_upload(committed.upload.id, committed.upload.owner, claimed.callback());
    ASSERT_TRUE(ulw::test::wait(*reactor, claimed));
    ASSERT_TRUE(call([&](auto done) {
        catalog->record_progress(committed.upload.id, committed.video.id, 3 * kChunk,
                                 std::move(done));
    }));
    ASSERT_TRUE(call([&](auto done) {
        catalog->commit_upload(committed.upload.id, committed.video.id, "req-1", std::move(done));
    }));
    ASSERT_TRUE(
        call([&](auto done) { catalog->abort_upload(discarded.upload.id, std::move(done)); }));

    const auto expired = reaper->expire(after_ttl(), 10);
    ASSERT_TRUE(expired);
    EXPECT_TRUE(expired->empty());
    EXPECT_EQ(upload_state(committed), "completed");
    EXPECT_EQ(video_state(committed), "processing");
    EXPECT_EQ(video_state(discarded), "init");
}

TEST_F(UploadReaperTest, SkipsAnUploadWhileItsOwnerHoldsTheClaimAndTakesItAfter) {
    const NewUpload u = created();
    Reply<core::ports::StoredUpload> claimed;
    catalog->claim_upload(u.upload.id, u.upload.owner, claimed.callback());
    ASSERT_TRUE(ulw::test::wait(*reactor, claimed));

    auto busy = reaper->expire(after_ttl(), 10);
    ASSERT_TRUE(busy);
    EXPECT_TRUE(busy->empty());
    EXPECT_EQ(upload_state(u), "active");

    catalog->release_upload(u.upload.id);
    // The unlock goes out on the catalog's own session; the reactor sends it.
    std::size_t taken = 0;
    ASSERT_TRUE(ulw::test::pump_until(*reactor, [&] {
        const auto again = reaper->expire(after_ttl(), 10);
        taken = again ? again->size() : 0;
        return taken != 0;
    }));
    EXPECT_EQ(upload_state(u), "aborted");
}

TEST_F(UploadReaperTest, AnUploadCommittedWhileTheReaperWaitedForItsRowIsLeftAlone) {
    const NewUpload u = created();
    auto committer = db->session();
    ASSERT_TRUE(committer.exec("BEGIN"));
    // What CommitUpload does, held open: the reaper's candidate query still sees 'active', and
    // its update then waits for this row.
    ASSERT_TRUE(committer.exec(
        "UPDATE uploads SET state = 'completed', durable_offset = size_bytes WHERE id = $1",
        Params{}.add_uuid(u.upload.id.uuid())));
    ASSERT_TRUE(committer.exec("UPDATE videos SET state = 'processing' WHERE id = $1",
                               Params{}.add_uuid(u.video.id.uuid())));

    auto expiry = std::async(std::launch::async, [&] { return reaper->expire(after_ttl(), 10); });
    auto watcher = db->session();
    bool waiting = false;
    for (int i = 0; i < 2000 && !waiting; ++i) {
        waiting =
            scalar(watcher,
                   "SELECT count(*) FROM pg_stat_activity "
                   "WHERE application_name = 'ulw-reaper' AND wait_event_type = 'Lock'") == "1";
        if (!waiting &&
            expiry.wait_for(std::chrono::milliseconds(5)) == std::future_status::ready) {
            break;
        }
    }
    ASSERT_TRUE(waiting) << "the reaper never reached the row lock";
    ASSERT_TRUE(committer.exec("COMMIT"));

    const auto expired = expiry.get();
    ASSERT_TRUE(expired);
    EXPECT_TRUE(expired->empty());
    EXPECT_EQ(upload_state(u), "completed");
    EXPECT_EQ(video_state(u), "processing");
}

TEST_F(UploadReaperTest, ACommitAfterTheReaperIsRefusedNotLost) {
    const NewUpload u = created();
    ASSERT_TRUE(reaper->expire(after_ttl(), 10));
    const auto committed = call([&](auto done) {
        catalog->commit_upload(u.upload.id, u.video.id, "req-1", std::move(done));
    });
    ASSERT_FALSE(committed);
    EXPECT_EQ(committed.error(), CatalogError::Conflict);
    auto conn = db->session();
    EXPECT_EQ(scalar(conn, "SELECT count(*) FROM jobs"), "0");
}

TEST_F(UploadReaperTest, AnUnreachableDatabaseIsUnavailable) {
    PgUploadReaper lost("host=127.0.0.1 port=1 connect_timeout=1");
    const auto expired = lost.expire(after_ttl(), 10);
    ASSERT_FALSE(expired);
    EXPECT_EQ(expired.error(), CatalogError::Unavailable);
}

// What a refused join leaves (ADR-0054's kind record, migration 0009) goes once it is a day old;
// a room anything uses stays, however old.
TEST_F(UploadReaperTest, ForgetsOnlyChatRoomsNothingUsedRecordedInTheWeekBeforeTheCutoff) {
    auto conn = db->session();
    const auto room = [&](std::string_view id, std::string_view age) {
        ASSERT_TRUE(conn.exec("INSERT INTO chat_rooms (room_id, kind, recorded_at) "
                              "VALUES ($1::text::uuid, 'group_chat', now() - $2::text::interval)",
                              Params{}.add_text(id).add_text(age)));
    };
    constexpr std::string_view kUnused = "01a0eb86-6cca-7dce-84cc-000000000001";
    constexpr std::string_view kFresh = "01a0eb86-6cca-7dce-84cc-000000000002";
    constexpr std::string_view kListed = "01a0eb86-6cca-7dce-84cc-000000000003";
    constexpr std::string_view kResolved = "01a0eb86-6cca-7dce-84cc-000000000004";
    constexpr std::string_view kAncient = "01a0eb86-6cca-7dce-84cc-000000000005";
    constexpr std::string_view kUnused2 = "01a0eb86-6cca-7dce-84cc-000000000006";
    room(kUnused, "2 days");
    room(kUnused2, "3 days");
    room(kFresh, "1 hour");
    room(kListed, "2 days");
    room(kResolved, "2 days");
    room(kAncient, "30 days");
    ASSERT_TRUE(
        conn.exec("INSERT INTO chat_members (room_id, user_id) VALUES ($1::text::uuid, 'alice')",
                  Params{}.add_text(kListed)));
    ASSERT_TRUE(conn.exec("INSERT INTO room_assignments (room_id, owner_node) "
                          "VALUES ($1::text::uuid, 'chat-1')",
                          Params{}.add_text(kResolved)));

    const auto cutoff = clock.wall_now() - std::chrono::hours(24);
    // Oldest first, a batch at a time.
    const auto first = reaper->forget_unused(cutoff, 1);
    ASSERT_TRUE(first);
    EXPECT_EQ(*first, 1U);
    EXPECT_EQ(scalar(conn, "SELECT count(*) FROM chat_rooms WHERE room_id = $1::text::uuid",
                     Params{}.add_text(kUnused2)),
              "0");
    const auto rest = reaper->forget_unused(cutoff, 10);
    ASSERT_TRUE(rest);
    EXPECT_EQ(*rest, 1U);
    const auto again = reaper->forget_unused(cutoff, 10);
    ASSERT_TRUE(again);
    EXPECT_EQ(*again, 0U);
    for (const std::string_view kept : {kFresh, kListed, kResolved, kAncient}) {
        EXPECT_EQ(scalar(conn, "SELECT count(*) FROM chat_rooms WHERE room_id = $1::text::uuid",
                         Params{}.add_text(kept)),
                  "1")
            << kept;
    }
}

} // namespace
