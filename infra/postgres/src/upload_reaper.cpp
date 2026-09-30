#include "infra/postgres/upload_reaper.hpp"

#include "lock_key.hpp"
#include "params.hpp"
#include "result.hpp"
#include "sql.hpp"
#include "sync_connection.hpp"

#include <chrono>
#include <optional>
#include <utility>

namespace infra::postgres {

namespace {

using core::ports::CatalogError;
using core::ports::ExpiredUpload;

// A few seconds is ample for an indexed statement on a small table; a longer wait means the
// server is unwell, and the next pass tries again.
constexpr SessionSettings kReaperSession{.application_name = "ulw-reaper",
                                         .statement_timeout = core::Millis{10'000}};

// Timestamps travel as integer microseconds since the epoch, as in the catalog.
constexpr Sql kCandidates = R"sql(
SELECT id, object_key, backend_ref, size_bytes, chunk_size
  FROM uploads
 WHERE state = 'active' AND expires_at <= timestamptz 'epoch' + $1 * interval '1 microsecond'
 ORDER BY expires_at
 LIMIT $2)sql";

// The abort, the video's failure and the check that nobody is streaming into the upload are
// one statement. The gateway holds a session-level advisory lock on lock_key(id) for as long
// as a request appends to an upload; the transaction-level lock taken here conflicts with it
// and is released with the statement, so a busy upload is skipped rather than waited for. The
// state and expiry are tested again on the row itself: a commit that got in after the
// candidates were read leaves it 'completed', and this updates nothing.
constexpr Sql kExpire = R"sql(
WITH expired AS (
    UPDATE uploads SET state = 'aborted'
     WHERE id = $1 AND state = 'active'
       AND expires_at <= timestamptz 'epoch' + $3 * interval '1 microsecond'
       AND pg_try_advisory_xact_lock($2)
    RETURNING video_id),
failed AS (
    UPDATE videos
       SET state = 'failed', error_reason = 'upload expired', version = version + 1,
           updated_at = now()
     WHERE id IN (SELECT video_id FROM expired) AND state IN ('init', 'uploading'))
SELECT count(*) FROM expired)sql";

// A room some join recorded and nothing used: no member, and never resolved on the room plane
// (room_assignments), which is where every message's seq comes from; chat_messages is asked too,
// for a room whose plane rows an operator removed. Rows another statement holds are skipped for
// the next pass. Only the last week is walked, oldest first, so a pass costs what a week of
// rooms does, not every room there ever was. A member added between the check and the delete
// is left in a room with no kind recorded, which admits only members and is recorded closed by
// the next join, as any room with members is.
constexpr Sql kForgetUnused = R"sql(
WITH unused AS (
    SELECT room_id FROM chat_rooms r
     WHERE recorded_at <= timestamptz 'epoch' + $1 * interval '1 microsecond'
       AND recorded_at > timestamptz 'epoch' + $1 * interval '1 microsecond' - interval '7 days'
       AND NOT EXISTS (SELECT 1 FROM chat_members m WHERE m.room_id = r.room_id)
       AND NOT EXISTS (SELECT 1 FROM room_assignments a WHERE a.room_id = r.room_id)
       AND NOT EXISTS (SELECT 1 FROM chat_messages c WHERE c.room_id = r.room_id)
     ORDER BY recorded_at
     LIMIT $2
       FOR UPDATE SKIP LOCKED)
DELETE FROM chat_rooms WHERE room_id IN (SELECT room_id FROM unused))sql";

CatalogError to_catalog_error(DbError e) noexcept {
    switch (e) {
    case DbError::Duplicate:
    case DbError::Constraint:
        return CatalogError::Conflict;
    case DbError::Retry:
    case DbError::ConnectionLost:
    case DbError::Timeout:
    case DbError::LockTimeout:
    case DbError::Rejected:
        return CatalogError::Unavailable;
    }
    return CatalogError::Unavailable;
}

std::unexpected<CatalogError> failure(const DbFailure& f) {
    return std::unexpected(to_catalog_error(f.error));
}

std::optional<ExpiredUpload> decode_candidate(const Result& rows, int row) {
    const auto id = domain_at<core::UploadId>(rows, row, 0);
    auto key = domain_at<core::StorageKey>(rows, row, 1);
    const auto backend_ref = rows.get(row, 2);
    const auto size = rows.get(row, 3).and_then(parse_uint64);
    const auto chunk = rows.get(row, 4).and_then(parse_uint64);
    if (!id || !key || !backend_ref || !size || !chunk) {
        return std::nullopt;
    }
    return ExpiredUpload{.id = *id,
                         .ingest = {.key = std::move(*key),
                                    .backend_ref = std::string{*backend_ref},
                                    .total_bytes = *size,
                                    .chunk_size = *chunk}};
}

std::int64_t micros_since_epoch(core::WallTime t) noexcept {
    return std::chrono::floor<std::chrono::microseconds>(t.time_since_epoch()).count();
}

} // namespace

class PgUploadReaper::Impl {
public:
    explicit Impl(std::string conninfo) : conninfo_(std::move(conninfo)) {}

    std::expected<SyncConnection*, CatalogError> session() {
        if (conn_ && !conn_->broken()) {
            return &*conn_;
        }
        conn_.reset();
        auto opened = SyncConnection::open(conninfo_, kReaperSession);
        if (!opened) {
            return failure(opened.error());
        }
        conn_.emplace(std::move(*opened));
        return &*conn_;
    }

private:
    std::string conninfo_;
    std::optional<SyncConnection> conn_;
};

PgUploadReaper::PgUploadReaper(std::string conninfo)
    : impl_(std::make_unique<Impl>(std::move(conninfo))) {}

PgUploadReaper::~PgUploadReaper() = default;

std::expected<std::vector<ExpiredUpload>, CatalogError> PgUploadReaper::expire(core::WallTime now,
                                                                               std::size_t limit) {
    auto conn = impl_->session();
    if (!conn) {
        return std::unexpected(conn.error());
    }
    const std::int64_t at = micros_since_epoch(now);
    auto candidates =
        (*conn)->exec(kCandidates, Params{}.add_int(at).add_int(static_cast<std::int64_t>(limit)));
    if (!candidates) {
        return failure(candidates.error());
    }
    std::vector<ExpiredUpload> expired;
    for (int row = 0; row < candidates->rows(); ++row) {
        auto upload = decode_candidate(*candidates, row);
        if (!upload) {
            // A row that violates the schema's own formats is not ours to abort or to skip
            // silently.
            return std::unexpected(CatalogError::Corrupt);
        }
        auto done = (*conn)->exec(
            kExpire,
            Params{}.add_uuid(upload->id.uuid()).add_int(lock_key(upload->id)).add_int(at));
        if (!done) {
            return failure(done.error());
        }
        if (done->get(0, 0).and_then(parse_uint64) == std::uint64_t{1}) {
            expired.push_back(std::move(*upload));
        }
    }
    return expired;
}

std::expected<std::size_t, CatalogError>
PgUploadReaper::forget_unused(core::WallTime recorded_before, std::size_t limit) {
    auto conn = impl_->session();
    if (!conn) {
        return std::unexpected(conn.error());
    }
    auto done = (*conn)->exec(kForgetUnused, Params{}
                                                 .add_int(micros_since_epoch(recorded_before))
                                                 .add_int(static_cast<std::int64_t>(limit)));
    if (!done) {
        return failure(done.error());
    }
    return done->affected();
}

} // namespace infra::postgres
