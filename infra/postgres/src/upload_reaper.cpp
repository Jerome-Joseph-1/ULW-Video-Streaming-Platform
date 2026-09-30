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

// A room some join recorded and nothing used: a direct or group chat (the only kinds a join
// records; a stream's live chat is opened by the server and lists nobody) with no member, never
// resolved on the room plane (room_assignments), which is where every message's seq comes from;
// chat_messages is asked too, for a room whose plane rows an operator removed. Each call walks at
// most $2 rooms recorded before the cutoff, in (recorded_at, room_id) order from the cursor the
// last call left, so a call costs the same however many rooms there are and every room is looked
// at again once a lap ends; the cursor starts over when a call finds fewer than $2. Rows another
// statement holds are skipped until the next lap. A member added between the check and the
// delete is left in a room with no kind recorded, which admits only members and is recorded
// closed by the next join, as any room with members is. Answers the rooms forgotten and whether
// the lap ended.
constexpr Sql kForgetUnused = R"sql(
WITH cursor AS (
    SELECT recorded_at, room_id FROM chat_rooms_forget_cursor FOR UPDATE),
walked AS (
    SELECT r.room_id, r.recorded_at FROM chat_rooms r, cursor c
     WHERE r.recorded_at <= timestamptz 'epoch' + $1 * interval '1 microsecond'
       AND (r.recorded_at, r.room_id) > (c.recorded_at, c.room_id)
     ORDER BY r.recorded_at, r.room_id
     LIMIT $2),
unused AS (
    SELECT room_id FROM chat_rooms r
     WHERE room_id IN (SELECT room_id FROM walked)
       AND kind IN ('direct_chat', 'group_chat')
       AND NOT EXISTS (SELECT 1 FROM chat_members m WHERE m.room_id = r.room_id)
       AND NOT EXISTS (SELECT 1 FROM room_assignments a WHERE a.room_id = r.room_id)
       AND NOT EXISTS (SELECT 1 FROM chat_messages c WHERE c.room_id = r.room_id)
       FOR UPDATE SKIP LOCKED),
gone AS (
    DELETE FROM chat_rooms WHERE room_id IN (SELECT room_id FROM unused) RETURNING 1),
last AS (
    SELECT recorded_at, room_id FROM walked ORDER BY recorded_at DESC, room_id DESC LIMIT 1),
lap AS (
    SELECT count(*) < $2 AS finished FROM walked),
moved AS (
    UPDATE chat_rooms_forget_cursor
       SET (recorded_at, room_id) =
           (SELECT CASE WHEN lap.finished THEN timestamptz '-infinity' ELSE last.recorded_at END,
                   CASE WHEN lap.finished THEN uuid '00000000-0000-0000-0000-000000000000'
                        ELSE last.room_id END
              FROM lap LEFT JOIN last ON true)
    RETURNING 1)
SELECT (SELECT count(*) FROM gone), (SELECT finished FROM lap), (SELECT count(*) FROM moved))sql";

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

std::expected<core::ports::UnusedRoomsScan, CatalogError>
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
    const auto forgotten = done->get(0, 0).and_then(parse_uint64);
    const auto finished = done->get(0, 1);
    // The cursor's row is made by the migration, and nothing deletes it.
    if (!forgotten || !finished || done->get(0, 2) != "1") {
        return std::unexpected(CatalogError::Corrupt);
    }
    return core::ports::UnusedRoomsScan{.forgotten = static_cast<std::size_t>(*forgotten),
                                        .finished = *finished == "t"};
}

} // namespace infra::postgres
