#include "infra/postgres/job_queue.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "postgres_harness.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using core::ports::ClaimedJob;
using core::ports::Rendition;
using infra::postgres::Params;
using infra::postgres::PgJobQueue;
using infra::postgres::SyncConnection;
using ulw::test::scalar;
using ulw::test::ScratchDatabase;

core::NodeId node(std::string_view name) {
    return *core::NodeId::parse(name);
}

class JobQueueTest : public ::testing::Test {
protected:
    void SetUp() override {
        ScratchDatabase::open(db);
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        conn.emplace(db->session());
        queue = std::make_unique<PgJobQueue>(db->conninfo());
    }

    // A video in processing with its transcode job queued, as a commit leaves them.
    core::VideoId queue_job() {
        const auto video = core::VideoId::generate(clock, random);
        const std::string key = "uploads/" + video.to_string();
        EXPECT_TRUE(conn->exec("INSERT INTO videos (id, owner_id, title, state) "
                               "VALUES ($1, 'auth0|tester', 'clip', 'processing')",
                               Params{}.add_uuid(video.uuid())));
        EXPECT_TRUE(conn->exec("INSERT INTO jobs (video_id, kind, source_key, request_id) "
                               "VALUES ($1, 'transcode', $2, 'req-1')",
                               Params{}.add_uuid(video.uuid()).add_text(key)));
        return video;
    }

    // nullopt, with a failure recorded, when no job could be claimed.
    static std::optional<ClaimedJob> claim(PgJobQueue& q, std::string_view worker) {
        auto claimed = q.claim(node(worker));
        if (!claimed || !*claimed) {
            ADD_FAILURE() << "no job claimed";
            return std::nullopt;
        }
        return std::move(*claimed);
    }

    void expire_leases() {
        ASSERT_TRUE(conn->exec("UPDATE jobs SET lease_expires = now() - interval '1 second'"));
    }
    void make_due() { ASSERT_TRUE(conn->exec("UPDATE jobs SET run_after = now()")); }

    std::string video_column(const core::VideoId& video, infra::postgres::Sql sql) {
        return scalar(*conn, sql, Params{}.add_uuid(video.uuid()));
    }

    os::SystemClock clock;
    os::SystemRandom random;
    std::unique_ptr<ScratchDatabase> db;
    std::optional<SyncConnection> conn;
    std::unique_ptr<PgJobQueue> queue;
};

TEST_F(JobQueueTest, ClaimLeasesTheOldestDueJob) {
    const auto first = queue_job();
    queue_job();
    const auto job = claim(*queue, "worker-a");
    ASSERT_TRUE(job);
    EXPECT_EQ(job->video, first);
    EXPECT_EQ(job->lease.fence, 1U);
    EXPECT_EQ(job->kind, core::ports::JobKind::Transcode);
    EXPECT_EQ(job->source.view(), "uploads/" + first.to_string());
    EXPECT_EQ(job->request_id, "req-1");
    const auto row = conn->exec("SELECT state, locked_by, attempts, "
                                "lease_expires > now() + interval '50 seconds' FROM jobs "
                                "WHERE id = $1",
                                Params{}.add_int(std::to_underlying(job->lease.job)));
    ASSERT_TRUE(row);
    EXPECT_EQ(row->get(0, 0), "running");
    EXPECT_EQ(row->get(0, 1), "worker-a");
    EXPECT_EQ(row->get(0, 2), "1");
    EXPECT_EQ(row->get(0, 3), "t");
}

TEST_F(JobQueueTest, NothingIsClaimedBeforeItIsDue) {
    queue_job();
    ASSERT_TRUE(conn->exec("UPDATE jobs SET run_after = now() + interval '1 hour'"));
    const auto claimed = queue->claim(node("worker-a"));
    ASSERT_TRUE(claimed);
    EXPECT_FALSE(*claimed);
}

TEST_F(JobQueueTest, ConcurrentClaimersNeverShareAJob) {
    constexpr int kJobs = 24;
    constexpr int kWorkers = 4;
    for (int i = 0; i < kJobs; ++i) {
        queue_job();
    }
    std::atomic<int> claimed{0};
    std::atomic<bool> broken{false};
    std::mutex mutex;
    std::vector<std::int64_t> ids;
    std::latch start(kWorkers);
    {
        std::vector<std::jthread> workers;
        workers.reserve(kWorkers);
        for (int w = 0; w < kWorkers; ++w) {
            workers.emplace_back([&, w] {
                PgJobQueue mine(db->conninfo());
                const auto me = node("worker-" + std::to_string(w));
                start.arrive_and_wait();
                while (claimed.load() < kJobs && !broken.load()) {
                    auto job = mine.claim(me);
                    if (!job) {
                        broken = true;
                        ADD_FAILURE() << "claim failed";
                        return;
                    }
                    if (*job) {
                        ++claimed;
                        const std::scoped_lock lock(mutex);
                        ids.push_back(std::to_underlying((*job)->lease.job));
                    }
                }
            });
        }
    }
    ASSERT_EQ(ids.size(), static_cast<std::size_t>(kJobs));
    std::ranges::sort(ids);
    EXPECT_EQ(std::ranges::adjacent_find(ids), ids.end());
}

TEST_F(JobQueueTest, FencedOutWorkerCannotTouchTheJobItLost) {
    const auto video = queue_job();
    const auto a = claim(*queue, "worker-a");
    ASSERT_TRUE(a);

    expire_leases();
    const auto reaped = queue->reap_expired();
    ASSERT_TRUE(reaped);
    EXPECT_EQ(*reaped, 1U);
    make_due();
    PgJobQueue other(db->conninfo());
    const auto b = claim(other, "worker-b");
    ASSERT_TRUE(b);
    ASSERT_EQ(b->lease.job, a->lease.job);
    EXPECT_GT(b->lease.fence, a->lease.fence);

    const std::array renditions{Rendition{.height = 720,
                                          .bitrate_bps = 3'000'000,
                                          .playlist = *core::StorageKey::parse("v/720.m3u8")}};
    EXPECT_EQ(queue->heartbeat(a->lease, node("worker-a")), false);
    EXPECT_EQ(queue->report_progress(a->lease, 50), false);
    EXPECT_EQ(queue->finish(a->lease, core::Millis{1000}, renditions), false);
    EXPECT_EQ(queue->fail(a->lease, "ffmpeg crashed", false), false);
    EXPECT_EQ(video_column(video, "SELECT state::text FROM videos WHERE id = $1"), "processing");
    EXPECT_EQ(video_column(video, "SELECT count(*) FROM renditions WHERE video_id = $1"), "0");

    EXPECT_EQ(other.heartbeat(b->lease, node("worker-b")), true);
    EXPECT_EQ(other.finish(b->lease, core::Millis{42'000}, renditions), true);
    EXPECT_EQ(video_column(video, "SELECT state::text FROM videos WHERE id = $1"), "ready");
    EXPECT_EQ(video_column(video, "SELECT duration_ms FROM videos WHERE id = $1"), "42000");
    EXPECT_EQ(video_column(video, "SELECT state FROM jobs WHERE video_id = $1"), "done");
}

TEST_F(JobQueueTest, FinishRecordsEveryRenditionAndReadiesTheVideo) {
    const auto video = queue_job();
    const auto job = claim(*queue, "worker-a");
    ASSERT_TRUE(job);
    const std::array renditions{
        Rendition{.height = 360,
                  .bitrate_bps = 800'000,
                  .playlist = *core::StorageKey::parse("v/360.m3u8")},
        Rendition{.height = 720,
                  .bitrate_bps = 3'000'000,
                  .playlist = *core::StorageKey::parse("v/720.m3u8")},
        Rendition{.height = 1080,
                  .bitrate_bps = 6'000'000,
                  .playlist = *core::StorageKey::parse("v/1080.m3u8")},
    };
    EXPECT_EQ(queue->finish(job->lease, core::Millis{61'250}, renditions), true);
    EXPECT_EQ(video_column(video, "SELECT string_agg(height || ':' || playlist_key, ',' "
                                  "ORDER BY height) FROM renditions WHERE video_id = $1"),
              "360:v/360.m3u8,720:v/720.m3u8,1080:v/1080.m3u8");
    EXPECT_EQ(video_column(video, "SELECT version FROM videos WHERE id = $1"), "1");
    EXPECT_EQ(queue->finish(job->lease, core::Millis{61'250}, renditions), false);
}

TEST_F(JobQueueTest, ReaperFailsTheJobAndItsVideoTogetherOnceAttemptsRunOut) {
    const auto video = queue_job();
    ASSERT_TRUE(conn->exec("UPDATE jobs SET max_attempts = 1"));
    ASSERT_TRUE(claim(*queue, "worker-a"));
    expire_leases();
    EXPECT_EQ(queue->reap_expired(), 1U);
    const auto row = conn->exec("SELECT j.state, j.last_error, v.state::text, v.error_reason "
                                "FROM jobs j JOIN videos v ON v.id = j.video_id "
                                "WHERE v.id = $1",
                                Params{}.add_uuid(video.uuid()));
    ASSERT_TRUE(row);
    EXPECT_EQ(row->get(0, 0), "failed");
    EXPECT_EQ(row->get(0, 1), "worker lease expired");
    EXPECT_EQ(row->get(0, 2), "failed");
    EXPECT_EQ(row->get(0, 3), "transcoding stopped responding");
    EXPECT_EQ(queue->reap_expired(), 0U);
}

TEST_F(JobQueueTest, ReaperRequeuesWithABackoffThatDoublesPerAttempt) {
    queue_job();
    ASSERT_TRUE(claim(*queue, "worker-a"));
    expire_leases();
    EXPECT_EQ(queue->reap_expired(), 1U);
    // 10 s x 2^1 after the first attempt.
    EXPECT_EQ(scalar(*conn, "SELECT state = 'queued' AND locked_by IS NULL AND run_after - now() "
                            "BETWEEN interval '19 seconds' AND interval '20 seconds' FROM jobs"),
              "t");
    const auto again = queue->claim(node("worker-a"));
    ASSERT_TRUE(again);
    EXPECT_FALSE(*again);
}

TEST_F(JobQueueTest, RetryableFailureRequeuesUntilAttemptsRunOut) {
    const auto video = queue_job();
    for (int attempt = 1; attempt < 3; ++attempt) {
        const auto job = claim(*queue, "worker-a");
        ASSERT_TRUE(job);
        EXPECT_EQ(queue->fail(job->lease, "source store unreachable", true), true);
        EXPECT_EQ(video_column(video, "SELECT state FROM jobs WHERE video_id = $1"), "queued");
        EXPECT_EQ(video_column(video, "SELECT state::text FROM videos WHERE id = $1"),
                  "processing");
        make_due();
    }
    const auto last = claim(*queue, "worker-a");
    ASSERT_TRUE(last);
    EXPECT_EQ(queue->fail(last->lease, "source store unreachable", true), true);
    EXPECT_EQ(video_column(video, "SELECT state FROM jobs WHERE video_id = $1"), "failed");
    EXPECT_EQ(video_column(video, "SELECT error_reason FROM videos WHERE id = $1"),
              "source store unreachable");
}

TEST_F(JobQueueTest, PermanentFailureFailsTheVideoAtOnce) {
    const auto video = queue_job();
    const auto job = claim(*queue, "worker-a");
    ASSERT_TRUE(job);
    EXPECT_EQ(queue->fail(job->lease, "not a video file", false), true);
    EXPECT_EQ(video_column(video, "SELECT state::text FROM videos WHERE id = $1"), "failed");
    EXPECT_EQ(video_column(video, "SELECT error_reason FROM videos WHERE id = $1"),
              "not a video file");
}

TEST_F(JobQueueTest, HeartbeatRenewsOnlyTheHoldersLease) {
    queue_job();
    const auto job = claim(*queue, "worker-a");
    ASSERT_TRUE(job);
    ASSERT_TRUE(conn->exec("UPDATE jobs SET lease_expires = now() + interval '1 second'"));
    EXPECT_EQ(queue->heartbeat(job->lease, node("worker-b")), false);
    EXPECT_EQ(queue->heartbeat(job->lease, node("worker-a")), true);
    EXPECT_EQ(scalar(*conn, "SELECT lease_expires > now() + interval '50 seconds' FROM jobs"), "t");
}

TEST_F(JobQueueTest, WaitForWorkWakesWhenAJobIsQueued) {
    // The first call opens the session and subscribes, so the notification cannot be missed.
    ASSERT_TRUE(queue->claim(node("worker-a")));
    auto waiting =
        std::async(std::launch::async, [&] { queue->wait_for_work(std::chrono::seconds(30)); });
    ASSERT_TRUE(conn->exec("NOTIFY job_available"));
    EXPECT_EQ(waiting.wait_for(std::chrono::seconds(10)), std::future_status::ready);
}

TEST_F(JobQueueTest, WaitForWorkReturnsAfterItsIntervalWithoutANotification) {
    auto waiting = std::async(std::launch::async,
                              [&] { queue->wait_for_work(std::chrono::milliseconds(200)); });
    EXPECT_EQ(waiting.wait_for(std::chrono::seconds(10)), std::future_status::ready);
}

TEST_F(JobQueueTest, CallAfterALostSessionReconnects) {
    queue_job();
    ASSERT_TRUE(queue->claim(node("worker-a")));
    ASSERT_NE(scalar(*conn, "SELECT count(pg_terminate_backend(pid)) FROM pg_stat_activity "
                            "WHERE datname = current_database() AND pid <> pg_backend_pid()"),
              "0");
    EXPECT_EQ(queue->reap_expired().error(), core::ports::JobQueueError::Unavailable);
    EXPECT_TRUE(queue->reap_expired());
}

} // namespace
