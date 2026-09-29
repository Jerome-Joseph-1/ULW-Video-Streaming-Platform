#include "infra/postgres/job_queue.hpp"

#include "job_session.hpp"
#include "params.hpp"
#include "result.hpp"
#include "sql.hpp"
#include "sync_connection.hpp"

#include <optional>
#include <thread>
#include <utility>

namespace infra::postgres {

namespace {

using core::ports::ClaimedJob;
using core::ports::JobId;
using core::ports::JobLease;
using core::ports::JobQueueError;
using core::ports::JobQueueResult;

// SKIP LOCKED: concurrent claimers pass over each other's rows instead of queueing behind
// them, so each gets a different job.
constexpr Sql kClaim = R"sql(
UPDATE jobs
   SET state = 'running', locked_by = $1, lease_expires = now() + $2 * interval '1 second',
       fence = fence + 1, attempts = attempts + 1
 WHERE id = (SELECT id FROM jobs
              WHERE state = 'queued' AND run_after <= now()
              ORDER BY id
              FOR UPDATE SKIP LOCKED
              LIMIT 1)
RETURNING id, video_id, kind, fence, source_key, request_id)sql";

// Every write by a holder also requires 'running': the reaper requeues or fails a job without
// raising its fence, and the lapsed holder must be shut out from that moment on.
constexpr Sql kHeartbeat = R"sql(
UPDATE jobs SET lease_expires = now() + $4 * interval '1 second'
 WHERE id = $1 AND locked_by = $2 AND fence = $3 AND state = 'running')sql";

constexpr Sql kProgress = R"sql(
UPDATE jobs SET progress_pct = $3 WHERE id = $1 AND fence = $2 AND state = 'running')sql";

constexpr Sql kFinishJob = R"sql(
UPDATE jobs SET state = 'done'
 WHERE id = $1 AND fence = $2 AND state = 'running'
RETURNING video_id)sql";

// The EXISTS repeats the fence check the job update just made in this transaction, so the
// video write stays fenced on its own if the statements are ever split or reordered.
constexpr Sql kVideoReady = R"sql(
UPDATE videos SET state = 'ready', duration_ms = $2, version = version + 1, updated_at = now()
 WHERE id = $1 AND state = 'processing'
   AND EXISTS (SELECT 1 FROM jobs WHERE id = $4 AND fence = $3))sql";

constexpr Sql kRendition = R"sql(
INSERT INTO renditions (video_id, height, bitrate_bps, playlist_key)
VALUES ($1, $2, $3, $4))sql";

// One statement, so a job and its video fail together or not at all. The retry delay doubles
// per attempt from 20 s, which gives a transient cause (a restarting store) time to clear.
constexpr Sql kFail = R"sql(
WITH job AS (
    UPDATE jobs
       SET state = CASE WHEN $4 AND attempts < max_attempts THEN 'queued' ELSE 'failed' END,
           run_after = now() + interval '10 seconds' * power(2, attempts),
           last_error = $3, locked_by = NULL, lease_expires = NULL
     WHERE id = $1 AND fence = $2 AND state = 'running'
    RETURNING video_id, state),
video AS (
    UPDATE videos
       SET state = 'failed', error_reason = $3, version = version + 1, updated_at = now()
     WHERE id IN (SELECT video_id FROM job WHERE state = 'failed') AND state = 'processing')
SELECT count(*) FROM job)sql";

constexpr Sql kReap = R"sql(
WITH expired AS (
    SELECT id FROM jobs
     WHERE state = 'running' AND lease_expires < now()
     FOR UPDATE SKIP LOCKED),
requeued AS (
    UPDATE jobs
       SET state = 'queued', locked_by = NULL, lease_expires = NULL,
           run_after = now() + interval '10 seconds' * power(2, attempts),
           last_error = 'worker lease expired'
     WHERE id IN (SELECT id FROM expired) AND attempts < max_attempts
    RETURNING id),
exhausted AS (
    UPDATE jobs
       SET state = 'failed', locked_by = NULL, lease_expires = NULL,
           last_error = 'worker lease expired'
     WHERE id IN (SELECT id FROM expired) AND attempts >= max_attempts
    RETURNING video_id),
video AS (
    UPDATE videos
       SET state = 'failed', error_reason = 'transcoding stopped responding',
           version = version + 1, updated_at = now()
     WHERE id IN (SELECT video_id FROM exhausted) AND state = 'processing')
SELECT (SELECT count(*) FROM requeued) + (SELECT count(*) FROM exhausted))sql";

JobQueueError to_queue_error(DbError e) noexcept {
    switch (e) {
    case DbError::Retry:
    case DbError::ConnectionLost:
    case DbError::Timeout:
    case DbError::LockTimeout:
        return JobQueueError::Unavailable;
    case DbError::Duplicate:
    case DbError::Constraint:
    case DbError::Rejected:
        return JobQueueError::Invalid;
    }
    return JobQueueError::Unavailable;
}

template <class T> JobQueueResult<T> failure(const DbFailure& f) {
    return std::unexpected(to_queue_error(f.error));
}

// Fences count the claims of one job and job ids count jobs; both stay far below 2^63.
Params& add_lease(Params& params, const JobLease& lease) noexcept {
    return params.add_int(std::to_underlying(lease.job))
        .add_int(static_cast<std::int64_t>(lease.fence));
}

std::optional<core::ports::JobKind> parse_kind(std::string_view text) noexcept {
    if (text == "transcode") {
        return core::ports::JobKind::Transcode;
    }
    return std::nullopt;
}

JobQueueResult<ClaimedJob> decode_claim(const Result& row) {
    const auto id = row.get(0, 0).and_then(parse_int64);
    const auto video = domain_at<core::VideoId>(row, 0, 1);
    const auto kind = row.get(0, 2).and_then(parse_kind);
    const auto fence = row.get(0, 3).and_then(parse_uint64);
    auto source = domain_at<core::StorageKey>(row, 0, 4);
    if (!id || !video || !kind || !fence || !source) {
        return std::unexpected(JobQueueError::Corrupt);
    }
    return ClaimedJob{.lease = JobLease{.job = JobId{*id}, .fence = *fence},
                      .video = *video,
                      .kind = *kind,
                      .source = std::move(*source),
                      .request_id = std::string{row.get(0, 5).value_or("")}};
}

JobQueueResult<std::size_t> decode_count(const Result& row) {
    const auto count = row.get(0, 0).and_then(parse_uint64);
    if (!count) {
        return std::unexpected(JobQueueError::Corrupt);
    }
    return static_cast<std::size_t>(*count);
}

} // namespace

class PgJobQueue::Impl {
public:
    explicit Impl(std::string conninfo) : conninfo_(std::move(conninfo)) {}

    // A session that broke is replaced here, on the call after the one that saw it break.
    JobQueueResult<SyncConnection*> session() {
        if (conn_ && !conn_->broken()) {
            return &*conn_;
        }
        conn_.reset();
        auto opened = SyncConnection::open(conninfo_, kJobSession);
        if (!opened) {
            return failure<SyncConnection*>(opened.error());
        }
        // Before the first claim: a job queued after a claim that found nothing must wake us.
        if (auto listening = opened->exec("LISTEN job_available"); !listening) {
            return failure<SyncConnection*>(listening.error());
        }
        conn_.emplace(std::move(*opened));
        return &*conn_;
    }

    JobQueueResult<Result> exec(Sql sql, const Params& params) {
        auto conn = session();
        if (!conn) {
            return std::unexpected(conn.error());
        }
        auto result = (*conn)->exec(sql, params);
        if (!result) {
            return failure<Result>(result.error());
        }
        return std::move(*result);
    }

    JobQueueResult<bool> exec_touching_one(Sql sql, const Params& params) {
        return exec(sql, params).transform([](const Result& r) { return r.affected() == 1; });
    }

private:
    std::string conninfo_;
    std::optional<SyncConnection> conn_;
};

PgJobQueue::PgJobQueue(std::string conninfo) : impl_(std::make_unique<Impl>(std::move(conninfo))) {}

PgJobQueue::~PgJobQueue() = default;

JobQueueResult<std::optional<ClaimedJob>> PgJobQueue::claim(const core::NodeId& worker) {
    auto claimed = impl_->exec(
        kClaim, Params{}.add_text(worker.view()).add_int(core::ports::kJobLease.count()));
    if (!claimed) {
        return std::unexpected(claimed.error());
    }
    if (claimed->rows() == 0) {
        return std::nullopt;
    }
    return decode_claim(*claimed);
}

JobQueueResult<bool> PgJobQueue::heartbeat(const JobLease& lease, const core::NodeId& worker) {
    return impl_->exec_touching_one(kHeartbeat, Params{}
                                                    .add_int(std::to_underlying(lease.job))
                                                    .add_text(worker.view())
                                                    .add_int(static_cast<std::int64_t>(lease.fence))
                                                    .add_int(core::ports::kJobLease.count()));
}

JobQueueResult<bool> PgJobQueue::report_progress(const JobLease& lease, std::uint8_t percent) {
    Params params;
    add_lease(params, lease).add_int(percent);
    return impl_->exec_touching_one(kProgress, params);
}

JobQueueResult<bool> PgJobQueue::finish(const JobLease& lease, core::Millis duration,
                                        std::span<const core::ports::Rendition> renditions) {
    auto conn = impl_->session();
    if (!conn) {
        return std::unexpected(conn.error());
    }
    SyncConnection& db = **conn;
    auto tx = Transaction::begin(db);
    if (!tx) {
        return failure<bool>(tx.error());
    }
    Params lease_params;
    add_lease(lease_params, lease);
    auto job = db.exec(kFinishJob, lease_params);
    if (!job) {
        return failure<bool>(job.error());
    }
    if (job->rows() == 0) {
        return false;
    }
    const auto video = domain_at<core::VideoId>(*job, 0, 0);
    if (!video) {
        return std::unexpected(JobQueueError::Corrupt);
    }
    auto ready = db.exec(kVideoReady, Params{}
                                          .add_uuid(video->uuid())
                                          .add_int(duration.count())
                                          .add_int(static_cast<std::int64_t>(lease.fence))
                                          .add_int(std::to_underlying(lease.job)));
    if (!ready) {
        return failure<bool>(ready.error());
    }
    // The job was running under our fence, so its video must be processing.
    if (ready->affected() != 1) {
        return std::unexpected(JobQueueError::Corrupt);
    }
    for (const core::ports::Rendition& r : renditions) {
        if (auto recorded = db.exec(kRendition, Params{}
                                                    .add_uuid(video->uuid())
                                                    .add_int(r.height)
                                                    .add_int(r.bitrate_bps)
                                                    .add_text(r.playlist.view()));
            !recorded) {
            return failure<bool>(recorded.error());
        }
    }
    if (auto committed = tx->commit(); !committed) {
        return failure<bool>(committed.error());
    }
    return true;
}

JobQueueResult<bool> PgJobQueue::fail(const JobLease& lease, std::string_view reason,
                                      bool retryable) {
    Params params;
    add_lease(params, lease).add_text(reason).add_bool(retryable);
    return impl_->exec(kFail, params).and_then(decode_count).transform([](std::size_t n) {
        return n == 1;
    });
}

JobQueueResult<std::size_t> PgJobQueue::reap_expired() {
    return impl_->exec(kReap, Params{}).and_then(decode_count);
}

void PgJobQueue::wait_for_work(core::Millis max_wait) {
    auto conn = impl_->session();
    if (!conn) {
        // With the database unreachable, sitting out the interval is the backoff.
        std::this_thread::sleep_for(max_wait);
        return;
    }
    // A failed wait has marked the session broken; the next call replaces it.
    [[maybe_unused]] const auto woken = (*conn)->wait_for_notification(max_wait);
}

} // namespace infra::postgres
