#include "core/models/upload.hpp"
#include "core/models/video.hpp"
#include "infra/postgres/upload_catalog.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "postgres_harness.hpp"
#include "support/reactor_harness.hpp"

#include <chrono>
#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

namespace {

using core::ports::CatalogError;
using core::ports::CatalogResult;
using core::ports::ClaimedUpload;
using core::ports::ClaimToken;
using core::ports::NewUpload;
using core::ports::StoredUpload;
using infra::postgres::CatalogConfig;
using infra::postgres::Params;
using infra::postgres::PgUploadCatalog;
using ulw::test::Reply;
using ulw::test::scalar;
using ulw::test::ScratchDatabase;

constexpr std::uint64_t kChunk = 8ULL << 20U;

core::UserId tester() {
    return *core::UserId::parse("auth0|tester");
}

class CatalogTest : public ::testing::TestWithParam<net::ReactorKind> {
protected:
    void SetUp() override {
        ScratchDatabase::open(db);
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        auto r = net::make_reactor(GetParam(), clock, 4096);
        ASSERT_TRUE(r) << "reactor: " << std::strerror(r.error());
        reactor = std::move(*r);
        auto p = net::OffloadPool::create(*reactor, 1);
        ASSERT_TRUE(p);
        offload = std::move(*p);
        catalog = make_catalog();
        ASSERT_TRUE(catalog);
    }

    void TearDown() override {
        // The catalogs require the offload pool stopped before they go.
        offload.reset();
        other.reset();
        catalog.reset();
        reactor.reset();
        db.reset();
    }

    std::unique_ptr<PgUploadCatalog> make_catalog() {
        auto made =
            PgUploadCatalog::create(*reactor, *offload, CatalogConfig{.conninfo = db->conninfo()});
        if (!made) {
            ADD_FAILURE() << made.error();
            return nullptr;
        }
        return std::move(*made);
    }

    NewUpload new_upload(std::uint64_t size = 3 * kChunk) {
        const auto video = core::VideoId::generate(clock, random);
        const auto owner = tester();
        return NewUpload{
            .video = core::VideoRecord{.id = video,
                                       .owner = owner,
                                       .title = "holiday, day one",
                                       .state = core::VideoState::Init,
                                       .version = 0,
                                       .error_reason = std::nullopt,
                                       .duration = std::nullopt},
            .upload =
                core::UploadRecord{.id = core::UploadId::generate(clock, random),
                                   .video_id = video,
                                   .owner = owner,
                                   .size_bytes = size,
                                   .chunk_size = kChunk,
                                   .durable_offset = 0,
                                   .state = core::UploadState::Active,
                                   // Postgres keeps microseconds.
                                   .expires_at = std::chrono::floor<std::chrono::microseconds>(
                                       clock.wall_now() + std::chrono::hours(24))},
            .backend_ref = "ingest-7f3a",
            .object_key = *core::StorageKey::parse("uploads/" + video.to_string())};
    }

    template <class T, class Start> CatalogResult<T> call(Start start) {
        Reply<T> reply;
        start(reply.callback());
        return ulw::test::wait(*reactor, reply);
    }

    CatalogResult<void> create(PgUploadCatalog& c, const NewUpload& u) {
        return call<void>([&](auto done) { c.create_upload(u, std::move(done)); });
    }
    CatalogResult<ClaimedUpload> claim(PgUploadCatalog& c, const core::UploadId& id,
                                       const core::UserId& owner = tester()) {
        return call<ClaimedUpload>([&](auto done) { c.claim_upload(id, owner, std::move(done)); });
    }
    CatalogResult<void> progress(const NewUpload& u, ClaimToken token, std::uint64_t offset) {
        return call<void>([&](auto done) {
            catalog->record_progress(u.upload.id, token, u.video.id, offset, std::move(done));
        });
    }
    CatalogResult<core::VideoState> commit(PgUploadCatalog& c, const NewUpload& u) {
        return call<core::VideoState>(
            [&](auto done) { c.commit_upload(u.upload.id, u.video.id, "req-1", std::move(done)); });
    }
    CatalogResult<void> abort(const NewUpload& u) {
        return call<void>([&](auto done) { catalog->abort_upload(u.upload.id, std::move(done)); });
    }
    CatalogResult<core::VideoRecord> video(const core::VideoId& id) {
        return call<core::VideoRecord>(
            [&](auto done) { catalog->find_video(id, std::move(done)); });
    }
    CatalogResult<StoredUpload> upload(const core::UploadId& id) {
        return call<StoredUpload>([&](auto done) { catalog->find_upload(id, std::move(done)); });
    }

    // Claims, then records progress up to the whole size: an upload ready to commit.
    void fill(const NewUpload& u) {
        const auto held = claim(*catalog, u.upload.id);
        ASSERT_TRUE(held);
        ASSERT_TRUE(progress(u, held->token, u.upload.size_bytes));
    }

    std::string jobs_for(const NewUpload& u) {
        auto conn = db->session();
        return scalar(conn, "SELECT count(*) FROM jobs WHERE video_id = $1",
                      Params{}.add_uuid(u.video.id.uuid()));
    }

    std::string upload_state(const NewUpload& u) {
        auto conn = db->session();
        return scalar(conn, "SELECT state FROM uploads WHERE id = $1",
                      Params{}.add_uuid(u.upload.id.uuid()));
    }

    os::SystemClock clock;
    os::SystemRandom random;
    std::unique_ptr<ScratchDatabase> db;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<net::OffloadPool> offload;
    std::unique_ptr<PgUploadCatalog> catalog;
    // A second gateway process, for claims and races between processes.
    std::unique_ptr<PgUploadCatalog> other;
};

TEST_P(CatalogTest, CreatedUploadReadsBackFieldForField) {
    const NewUpload u = new_upload();
    ASSERT_TRUE(create(*catalog, u));

    const auto stored = upload(u.upload.id);
    ASSERT_TRUE(stored) << core::ports::to_string(stored.error());
    EXPECT_EQ(stored->upload.id, u.upload.id);
    EXPECT_EQ(stored->upload.video_id, u.video.id);
    EXPECT_EQ(stored->upload.owner, u.upload.owner);
    EXPECT_EQ(stored->upload.size_bytes, u.upload.size_bytes);
    EXPECT_EQ(stored->upload.chunk_size, kChunk);
    EXPECT_EQ(stored->upload.durable_offset, 0U);
    EXPECT_EQ(stored->upload.state, core::UploadState::Active);
    EXPECT_EQ(stored->upload.expires_at, u.upload.expires_at);
    EXPECT_EQ(stored->backend_ref, u.backend_ref);
    EXPECT_EQ(stored->object_key, u.object_key);

    const auto v = video(u.video.id);
    ASSERT_TRUE(v);
    EXPECT_EQ(v->state, core::VideoState::Init);
    EXPECT_EQ(v->version, 0U);
    EXPECT_EQ(v->title, u.video.title);
    EXPECT_EQ(v->owner, u.video.owner);
}

TEST_P(CatalogTest, ReplayedCreateSucceedsWithoutASecondRow) {
    const NewUpload u = new_upload();
    ASSERT_TRUE(create(*catalog, u));
    EXPECT_TRUE(create(*catalog, u));
    auto conn = db->session();
    EXPECT_EQ(scalar(conn, "SELECT count(*) FROM videos"), "1");
    EXPECT_EQ(scalar(conn, "SELECT count(*) FROM uploads"), "1");
}

TEST_P(CatalogTest, UnknownIdsAreNotFound) {
    const NewUpload u = new_upload();
    EXPECT_EQ(upload(u.upload.id).error(), CatalogError::NotFound);
    EXPECT_EQ(video(u.video.id).error(), CatalogError::NotFound);
}

TEST_P(CatalogTest, StoredRowBreakingADomainRuleReadsAsCorrupt) {
    const NewUpload u = new_upload();
    ASSERT_TRUE(create(*catalog, u));
    auto conn = db->session();
    ASSERT_TRUE(conn.exec("UPDATE videos SET owner_id = 'has space' WHERE id = $1",
                          Params{}.add_uuid(u.video.id.uuid())));
    ASSERT_TRUE(conn.exec("UPDATE uploads SET chunk_size = 0 WHERE id = $1",
                          Params{}.add_uuid(u.upload.id.uuid())));
    EXPECT_EQ(video(u.video.id).error(), CatalogError::Corrupt);
    EXPECT_EQ(upload(u.upload.id).error(), CatalogError::Corrupt);
}

// Playback reads what the worker leaves: a ready video with its duration, a failed one with its
// reason.
TEST_P(CatalogTest, AReadyOrFailedVideoReadsBackWithItsDurationOrReason) {
    const NewUpload ready = new_upload();
    const NewUpload failed = new_upload();
    ASSERT_TRUE(create(*catalog, ready));
    ASSERT_TRUE(create(*catalog, failed));
    auto conn = db->session();
    ASSERT_TRUE(conn.exec("UPDATE videos SET state = 'ready', duration_ms = 83456, version = 3 "
                          "WHERE id = $1",
                          Params{}.add_uuid(ready.video.id.uuid())));
    ASSERT_TRUE(conn.exec("UPDATE videos SET state = 'failed', error_reason = 'not a video', "
                          "version = 2 WHERE id = $1",
                          Params{}.add_uuid(failed.video.id.uuid())));

    const auto r = video(ready.video.id);
    ASSERT_TRUE(r) << core::ports::to_string(r.error());
    EXPECT_EQ(r->state, core::VideoState::Ready);
    EXPECT_EQ(r->version, 3U);
    EXPECT_EQ(r->duration, core::Millis{83456});
    EXPECT_EQ(r->error_reason, std::nullopt);

    const auto f = video(failed.video.id);
    ASSERT_TRUE(f) << core::ports::to_string(f.error());
    EXPECT_EQ(f->state, core::VideoState::Failed);
    EXPECT_EQ(f->version, 2U);
    EXPECT_EQ(f->error_reason, "not a video");
    EXPECT_EQ(f->duration, std::nullopt);

    // The column takes any integer; a negative duration never reaches a player.
    ASSERT_TRUE(conn.exec("UPDATE videos SET duration_ms = -1 WHERE id = $1",
                          Params{}.add_uuid(ready.video.id.uuid())));
    EXPECT_EQ(video(ready.video.id).error(), CatalogError::Corrupt);
}

TEST_P(CatalogTest, FirstProgressStartsTheVideoAndOffsetsNeverMoveBack) {
    const NewUpload u = new_upload();
    ASSERT_TRUE(create(*catalog, u));
    const auto held = claim(*catalog, u.upload.id);
    ASSERT_TRUE(held);

    ASSERT_TRUE(progress(u, held->token, kChunk));
    const auto started = video(u.video.id);
    ASSERT_TRUE(started);
    EXPECT_EQ(started->state, core::VideoState::Uploading);
    EXPECT_EQ(started->version, 1U);

    ASSERT_TRUE(progress(u, held->token, 2 * kChunk));
    ASSERT_TRUE(progress(u, held->token, kChunk));
    EXPECT_EQ(upload(u.upload.id)->upload.durable_offset, 2 * kChunk);
    EXPECT_EQ(video(u.video.id)->version, 1U);
}

TEST_P(CatalogTest, ProgressWithoutAClaimIsRefused) {
    const NewUpload u = new_upload();
    ASSERT_TRUE(create(*catalog, u));
    EXPECT_EQ(progress(u, ClaimToken{}, kChunk).error(), CatalogError::Conflict);
    EXPECT_EQ(upload(u.upload.id)->upload.durable_offset, 0U);
}

TEST_P(CatalogTest, CommitCompletesTheUploadAndQueuesOneTranscode) {
    const NewUpload u = new_upload();
    ASSERT_TRUE(create(*catalog, u));
    fill(u);
    ASSERT_TRUE(commit(*catalog, u));

    const auto stored = upload(u.upload.id);
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->upload.state, core::UploadState::Completed);
    const auto v = video(u.video.id);
    ASSERT_TRUE(v);
    EXPECT_EQ(v->state, core::VideoState::Processing);
    EXPECT_EQ(v->version, 2U);
    auto conn = db->session();
    const auto job = conn.exec("SELECT kind, state, source_key, request_id FROM jobs "
                               "WHERE video_id = $1",
                               Params{}.add_uuid(u.video.id.uuid()));
    ASSERT_TRUE(job);
    ASSERT_EQ(job->rows(), 1);
    EXPECT_EQ(job->get(0, 0), "transcode");
    EXPECT_EQ(job->get(0, 1), "queued");
    EXPECT_EQ(job->get(0, 2), u.object_key.view());
    EXPECT_EQ(job->get(0, 3), "req-1");
}

TEST_P(CatalogTest, CommitCompletesAnUploadWhoseCachedOffsetLagged) {
    const NewUpload u = new_upload();
    ASSERT_TRUE(create(*catalog, u));
    ASSERT_TRUE(commit(*catalog, u));
    const auto stored = upload(u.upload.id);
    ASSERT_TRUE(stored) << core::ports::to_string(stored.error());
    EXPECT_EQ(stored->upload.durable_offset, u.upload.size_bytes);
}

TEST_P(CatalogTest, RepeatedCommitSucceedsWithoutASecondJob) {
    const NewUpload u = new_upload();
    ASSERT_TRUE(create(*catalog, u));
    fill(u);
    EXPECT_EQ(commit(*catalog, u), core::VideoState::Processing);
    EXPECT_EQ(commit(*catalog, u), core::VideoState::Processing);
    EXPECT_EQ(jobs_for(u), "1");

    // Once the job is finished no live job blocks an insert; the commit must not reach it. A
    // repeat answers the state the worker left.
    auto conn = db->session();
    ASSERT_TRUE(conn.exec("UPDATE jobs SET state = 'done' WHERE video_id = $1",
                          Params{}.add_uuid(u.video.id.uuid())));
    ASSERT_TRUE(conn.exec("UPDATE videos SET state = 'ready', duration_ms = 5000 WHERE id = $1",
                          Params{}.add_uuid(u.video.id.uuid())));
    EXPECT_EQ(commit(*catalog, u), core::VideoState::Ready);
    ASSERT_TRUE(conn.exec("UPDATE videos SET state = 'failed', duration_ms = NULL, "
                          "error_reason = 'rejected' WHERE id = $1",
                          Params{}.add_uuid(u.video.id.uuid())));
    EXPECT_EQ(commit(*catalog, u), core::VideoState::Failed);
    EXPECT_EQ(jobs_for(u), "1");
}

TEST_P(CatalogTest, ConcurrentCommitsFromTwoGatewaysQueueOneJob) {
    const NewUpload u = new_upload();
    ASSERT_TRUE(create(*catalog, u));
    fill(u);
    other = make_catalog();
    ASSERT_TRUE(other);
    Reply<core::VideoState> first;
    Reply<core::VideoState> second;
    catalog->commit_upload(u.upload.id, u.video.id, "req-1", first.callback());
    other->commit_upload(u.upload.id, u.video.id, "req-2", second.callback());
    EXPECT_EQ(ulw::test::wait(*reactor, first), core::VideoState::Processing);
    EXPECT_EQ(ulw::test::wait(*reactor, second), core::VideoState::Processing);
    EXPECT_EQ(jobs_for(u), "1");
}

TEST_P(CatalogTest, CommitThatCannotStartProcessingChangesNothing) {
    const NewUpload u = new_upload();
    ASSERT_TRUE(create(*catalog, u));
    fill(u);
    auto conn = db->session();
    ASSERT_TRUE(conn.exec("UPDATE videos SET state = 'failed', error_reason = 'rejected' "
                          "WHERE id = $1",
                          Params{}.add_uuid(u.video.id.uuid())));
    EXPECT_EQ(commit(*catalog, u).error(), CatalogError::Conflict);
    EXPECT_EQ(upload_state(u), "active");
    EXPECT_EQ(jobs_for(u), "0");
}

TEST_P(CatalogTest, CommitWakesListeningWorkers) {
    const NewUpload u = new_upload();
    ASSERT_TRUE(create(*catalog, u));
    fill(u);
    auto worker = db->session();
    ASSERT_TRUE(worker.exec("LISTEN job_available"));
    ASSERT_TRUE(commit(*catalog, u));
    const auto woke = worker.wait_for_notification(std::chrono::seconds(5));
    ASSERT_TRUE(woke);
    EXPECT_TRUE(*woke);
}

TEST_P(CatalogTest, CommitNeedsAnUploadThatWasNeitherAbortedNorLost) {
    const NewUpload u = new_upload();
    ASSERT_TRUE(create(*catalog, u));
    ASSERT_TRUE(abort(u));
    EXPECT_EQ(commit(*catalog, u).error(), CatalogError::Conflict);
    EXPECT_EQ(commit(*catalog, new_upload()).error(), CatalogError::NotFound);
    EXPECT_EQ(jobs_for(u), "0");
}

TEST_P(CatalogTest, AbortIsIdempotentButCannotUndoACommit) {
    const NewUpload aborted = new_upload();
    ASSERT_TRUE(create(*catalog, aborted));
    ASSERT_TRUE(abort(aborted));
    EXPECT_TRUE(abort(aborted));
    EXPECT_EQ(upload_state(aborted), "aborted");

    const NewUpload committed = new_upload();
    ASSERT_TRUE(create(*catalog, committed));
    ASSERT_TRUE(commit(*catalog, committed));
    EXPECT_EQ(abort(committed).error(), CatalogError::Conflict);
    EXPECT_EQ(abort(new_upload()).error(), CatalogError::NotFound);
}

TEST_P(CatalogTest, ClaimReturnsTheUploadAndRefusesASecondGateway) {
    const NewUpload u = new_upload();
    ASSERT_TRUE(create(*catalog, u));
    const auto held = claim(*catalog, u.upload.id);
    ASSERT_TRUE(held) << core::ports::to_string(held.error());
    EXPECT_EQ(held->stored.object_key, u.object_key);

    other = make_catalog();
    ASSERT_TRUE(other);
    EXPECT_EQ(claim(*other, u.upload.id).error(), CatalogError::Conflict);
    catalog->release_upload(u.upload.id, held->token);
    // The unlock travels on another session than the next claim; wait for it to land.
    CatalogResult<ClaimedUpload> taken = std::unexpected(CatalogError::Conflict);
    const bool claimed = ulw::test::pump_until(*reactor, [&] {
        taken = claim(*other, u.upload.id);
        return taken || taken.error() != CatalogError::Conflict;
    });
    ASSERT_TRUE(claimed);
    EXPECT_TRUE(taken);
}

TEST_P(CatalogTest, AClaimByAnotherUserIsNotFoundAndLocksNothing) {
    const NewUpload u = new_upload();
    ASSERT_TRUE(create(*catalog, u));
    EXPECT_EQ(claim(*catalog, u.upload.id, *core::UserId::parse("auth0|intruder")).error(),
              CatalogError::NotFound);
    other = make_catalog();
    ASSERT_TRUE(other);
    EXPECT_TRUE(claim(*other, u.upload.id));
}

TEST_P(CatalogTest, ClaimOutlivesTheOwnerItWasGiven) {
    const NewUpload u = new_upload();
    ASSERT_TRUE(create(*catalog, u));
    // The port asks callers to keep only the callback's captures alive. The owner goes before
    // the statement is sent, and another id of the same length takes over its memory.
    auto owner = std::make_unique<core::UserId>(tester());
    Reply<ClaimedUpload> reply;
    catalog->claim_upload(u.upload.id, *owner, reply.callback());
    owner.reset();
    const auto intruder = std::make_unique<core::UserId>(*core::UserId::parse("auth0|intrud"));
    const auto held = ulw::test::wait(*reactor, reply);
    ASSERT_TRUE(held) << core::ports::to_string(held.error());
    EXPECT_EQ(held->stored.upload.owner, tester());
}

TEST_P(CatalogTest, SecondClaimInOneProcessIsRefusedOnALaterIteration) {
    const NewUpload u = new_upload();
    ASSERT_TRUE(create(*catalog, u));
    ASSERT_TRUE(claim(*catalog, u.upload.id));
    Reply<ClaimedUpload> again;
    catalog->claim_upload(u.upload.id, u.upload.owner, again.callback());
    EXPECT_EQ(again.calls(), 0);
    EXPECT_EQ(ulw::test::wait(*reactor, again).error(), CatalogError::Conflict);
    EXPECT_EQ(again.calls(), 1);
}

TEST_P(CatalogTest, KilledGatewayFreesItsClaimsAtOnce) {
    const NewUpload u = new_upload();
    ASSERT_TRUE(create(*catalog, u));
    other = make_catalog();
    ASSERT_TRUE(other);
    ASSERT_TRUE(claim(*other, u.upload.id));
    ASSERT_EQ(claim(*catalog, u.upload.id).error(), CatalogError::Conflict);

    // Destroying it closes its sessions exactly as the kernel does for a killed process.
    other.reset();
    CatalogResult<ClaimedUpload> taken = std::unexpected(CatalogError::Conflict);
    const bool claimed = ulw::test::pump_until(*reactor, [&] {
        taken = claim(*catalog, u.upload.id);
        return taken || taken.error() != CatalogError::Conflict;
    });
    ASSERT_TRUE(claimed);
    EXPECT_TRUE(taken);
}

TEST_P(CatalogTest, LosingTheLockSessionLosesEveryClaim) {
    const NewUpload u = new_upload();
    ASSERT_TRUE(create(*catalog, u));
    const auto held = claim(*catalog, u.upload.id);
    ASSERT_TRUE(held);
    ASSERT_TRUE(progress(u, held->token, kChunk));

    auto conn = db->session();
    ASSERT_EQ(scalar(conn,
                     "SELECT count(pg_terminate_backend(pid)) FROM pg_stat_activity "
                     "WHERE datname = current_database() AND application_name = 'ulw-claims'"),
              "1");
    CatalogResult<void> recorded;
    const bool refused = ulw::test::pump_until(*reactor, [&] {
        recorded = progress(u, held->token, 2 * kChunk);
        return !recorded;
    });
    ASSERT_TRUE(refused);
    EXPECT_EQ(recorded.error(), CatalogError::Conflict);

    other = make_catalog();
    ASSERT_TRUE(other);
    EXPECT_TRUE(claim(*other, u.upload.id));
}

// The claim session is lost and the upload claimed again in the same process. The holder of
// the lost grant still has its token, and its late release must not give back the new grant:
// before releases named their grant, it did, and another gateway could then claim the upload
// under a holder that believed it held it.
TEST_P(CatalogTest, AReleaseFromTheHolderOfALostClaimLeavesTheNewClaimHeld) {
    const NewUpload u = new_upload();
    ASSERT_TRUE(create(*catalog, u));
    const auto lost = claim(*catalog, u.upload.id);
    ASSERT_TRUE(lost) << core::ports::to_string(lost.error());

    auto conn = db->session();
    ASSERT_EQ(scalar(conn,
                     "SELECT count(pg_terminate_backend(pid)) FROM pg_stat_activity "
                     "WHERE datname = current_database() AND application_name = 'ulw-claims'"),
              "1");
    CatalogResult<ClaimedUpload> again = std::unexpected(CatalogError::Unavailable);
    ASSERT_TRUE(ulw::test::pump_until(*reactor, [&] {
        again = claim(*catalog, u.upload.id);
        return again.has_value();
    }));
    EXPECT_NE(again->token, lost->token);

    // The old holder's append, still in flight, records nothing under the new grant.
    EXPECT_EQ(progress(u, lost->token, kChunk).error(), CatalogError::Conflict);
    catalog->release_upload(u.upload.id, lost->token);
    EXPECT_TRUE(progress(u, again->token, kChunk));
    other = make_catalog();
    ASSERT_TRUE(other);
    EXPECT_EQ(claim(*other, u.upload.id).error(), CatalogError::Conflict);

    catalog->release_upload(u.upload.id, again->token);
    EXPECT_EQ(progress(u, again->token, 2 * kChunk).error(), CatalogError::Conflict);
    CatalogResult<ClaimedUpload> taken = std::unexpected(CatalogError::Conflict);
    ASSERT_TRUE(ulw::test::pump_until(*reactor, [&] {
        taken = claim(*other, u.upload.id);
        return taken || taken.error() != CatalogError::Conflict;
    }));
    EXPECT_TRUE(taken);
}

TEST_P(CatalogTest, ClaimThatLoadsACorruptRowLetsGoOfItsLock) {
    const NewUpload u = new_upload();
    ASSERT_TRUE(create(*catalog, u));
    // The owner still matches, so the lock is taken; only the load afterwards fails.
    auto conn = db->session();
    ASSERT_TRUE(conn.exec("UPDATE uploads SET chunk_size = 0 WHERE id = $1",
                          Params{}.add_uuid(u.upload.id.uuid())));
    ASSERT_EQ(claim(*catalog, u.upload.id).error(), CatalogError::Corrupt);
    other = make_catalog();
    ASSERT_TRUE(other);
    // The unlock travels on another session than the next claim; wait for it to land.
    CatalogResult<ClaimedUpload> seen = std::unexpected(CatalogError::Conflict);
    const bool settled = ulw::test::pump_until(*reactor, [&] {
        seen = claim(*other, u.upload.id);
        return seen || seen.error() != CatalogError::Conflict;
    });
    ASSERT_TRUE(settled);
    EXPECT_EQ(seen.error(), CatalogError::Corrupt);
}

TEST_P(CatalogTest, ABatchOfViewsLandsInOneStatementFieldForField) {
    const auto a = core::VideoId::generate(clock, random);
    const auto b = core::VideoId::generate(clock, random);
    const auto at = std::chrono::floor<std::chrono::microseconds>(clock.wall_now());
    std::vector<core::ports::ViewEvent> batch{
        {.video = a, .viewer = tester(), .at = at},
        {.video = b,
         .viewer = *core::UserId::parse("user.b@example.com"),
         .at = at + std::chrono::microseconds(1)},
        {.video = a, .viewer = *core::UserId::parse("x:y+z-1.2"), .at = at},
        // Array literals read an unquoted NULL, in any case, as SQL NULL.
        {.video = a, .viewer = *core::UserId::parse("null"), .at = at},
        {.video = a, .viewer = *core::UserId::parse("NULL"), .at = at}};
    ASSERT_TRUE(call<void>([&](auto done) { catalog->record_views(batch, std::move(done)); }));

    auto conn = db->session();
    EXPECT_EQ(scalar(conn, "SELECT count(*) FROM view_events"), "5");
    EXPECT_EQ(
        scalar(
            conn,
            "SELECT string_agg(viewer_id, ' ' ORDER BY viewer_id COLLATE \"C\") FROM view_events "
            "WHERE video_id = $1",
            Params{}.add_uuid(a.uuid())),
        "NULL auth0|tester null x:y+z-1.2");
    EXPECT_EQ(
        scalar(conn,
               "SELECT (extract(epoch FROM viewed_at) * 1000000)::bigint FROM view_events "
               "WHERE video_id = $1",
               Params{}.add_uuid(b.uuid())),
        std::to_string(
            std::chrono::duration_cast<std::chrono::microseconds>(at.time_since_epoch()).count() +
            1));
}

TEST_P(CatalogTest, AnEmptyBatchOfViewsSucceedsWithoutAStatement) {
    EXPECT_TRUE(call<void>([&](auto done) { catalog->record_views({}, std::move(done)); }));
    auto conn = db->session();
    EXPECT_EQ(scalar(conn, "SELECT count(*) FROM view_events"), "0");
}

TEST_P(CatalogTest, RefusesConnectionStringsLibpqWouldResolveOnTheLoop) {
    for (const std::string conninfo :
         {"service=ulw dbname=ulw", "dbname=ulw user=ulw", "host='unterminated"}) {
        const auto made =
            PgUploadCatalog::create(*reactor, *offload, CatalogConfig{.conninfo = conninfo});
        EXPECT_FALSE(made) << conninfo;
    }
}

INSTANTIATE_TEST_SUITE_P(Reactors, CatalogTest,
                         ::testing::Values(net::ReactorKind::IoUring, net::ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
