#include "infra/postgres/upload_catalog.hpp"

#include "deferred_calls.hpp"
#include "lock_key.hpp"
#include "operation.hpp"
#include "pool.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace infra::postgres {

namespace {

using core::ports::CatalogCallback;
using core::ports::CatalogError;
using core::ports::CatalogResult;
using core::ports::ClaimedUpload;
using core::ports::ClaimToken;
using core::ports::NewUpload;
using core::ports::StoredUpload;

constexpr Sql kBegin = "BEGIN";
constexpr Sql kCommit = "COMMIT";

// One statement is one transaction: the video and its upload exist together or not at all.
constexpr Sql kCreateUpload = R"sql(
WITH video AS (
    INSERT INTO videos (id, owner_id, title) VALUES ($1, $2, $3)
    RETURNING id)
INSERT INTO uploads (id, video_id, owner_id, backend_ref, object_key, chunk_size, size_bytes,
                     expires_at)
SELECT $4, video.id, $5, $6, $7, $8, $9, timestamptz 'epoch' + $10 * interval '1 microsecond'
  FROM video)sql";

// Timestamps travel as integer microseconds since the epoch: exact, and free of DateStyle and
// TimeZone settings.
constexpr Sql kFindUpload = R"sql(
SELECT id, video_id, owner_id, size_bytes, chunk_size, durable_offset, state,
       (extract(epoch FROM expires_at) * 1000000)::bigint, backend_ref, object_key
  FROM uploads WHERE id = $1)sql";

// The video and what the viewer ($2) is to it, in one statement (ADR-0097). Every lookup goes
// by a primary key: chat_members (room_id, user_id) and video_grants (video_id, user_id). A
// room shares the video only while it lists both the viewer and the owner: an owner who left
// the room no longer shares into it. The owner needs neither, and gets false for both.
constexpr Sql kFindVideoFor = R"sql(
SELECT v.id, v.owner_id, v.title, v.state, v.version, v.error_reason, v.duration_ms,
       v.visibility, v.visibility_room,
       v.owner_id <> $2 AND v.visibility = 'room'
           AND EXISTS (SELECT 1 FROM chat_members m
                        WHERE m.room_id = v.visibility_room AND m.user_id = $2)
           AND EXISTS (SELECT 1 FROM chat_members o
                        WHERE o.room_id = v.visibility_room AND o.user_id = v.owner_id),
       v.owner_id <> $2
           AND EXISTS (SELECT 1 FROM video_grants g WHERE g.video_id = v.id AND g.user_id = $2)
  FROM videos v WHERE v.id = $1 AND v.deleted_at IS NULL)sql";

// The owner ($2) sets it, and a room only one chat_members lists them in at this moment. $4 is
// the room's id, or '' for a visibility without one.
constexpr Sql kSetVisibility = R"sql(
UPDATE videos SET visibility = $3, visibility_room = NULLIF($4, '')::uuid, updated_at = now()
 WHERE id = $1 AND owner_id = $2 AND deleted_at IS NULL
   AND ($4 = '' OR EXISTS (SELECT 1 FROM chat_members
                            WHERE room_id = NULLIF($4, '')::uuid AND user_id = $2))
RETURNING id, owner_id, title, state, version, error_reason, duration_ms, visibility,
          visibility_room)sql";

// Why kSetVisibility changed nothing: a video of the owner's (the room was refused) or none.
constexpr Sql kOwnsVideo =
    "SELECT 1 FROM videos WHERE id = $1 AND owner_id = $2 AND deleted_at IS NULL";

// Each answers whether the video exists, and grants or revokes in the same statement.
constexpr Sql kGrantAccess = R"sql(
WITH video AS (SELECT id FROM videos WHERE id = $1 AND deleted_at IS NULL),
granted AS (
    INSERT INTO video_grants (video_id, user_id) SELECT id, $2 FROM video
    ON CONFLICT (video_id, user_id) DO NOTHING)
SELECT count(*) FROM video)sql";

constexpr Sql kRevokeAccess = R"sql(
WITH video AS (SELECT id FROM videos WHERE id = $1 AND deleted_at IS NULL),
revoked AS (DELETE FROM video_grants WHERE video_id = $1 AND user_id = $2)
SELECT count(*) FROM video)sql";

// No row: no such video. One row with a NULL user: a video without grants after the cursor.
// user_id is COLLATE "C", so the order and the cursor are bytewise.
constexpr Sql kListGrants = R"sql(
SELECT g.user_id, (extract(epoch FROM g.granted_at) * 1000000)::bigint
  FROM videos v
  LEFT JOIN LATERAL (SELECT user_id, granted_at FROM video_grants
                      WHERE video_id = v.id AND user_id > $2
                      ORDER BY user_id LIMIT $3) g ON true
 WHERE v.id = $1 AND v.deleted_at IS NULL
 ORDER BY g.user_id)sql";

// The video's first transition rides on the progress write, in the same statement.
constexpr Sql kRecordProgress = R"sql(
WITH progressed AS (
    UPDATE uploads SET durable_offset = GREATEST(durable_offset, $2)
     WHERE id = $1 AND state = 'active'
    RETURNING video_id),
started AS (
    UPDATE videos SET state = 'uploading', version = version + 1, updated_at = now()
     WHERE id = $3 AND state = 'init' AND id IN (SELECT video_id FROM progressed))
SELECT count(*) FROM progressed)sql";

// The store has made every byte durable by the time an upload commits, so the cached offset
// catches up with it here.
constexpr Sql kCompleteUpload = R"sql(
UPDATE uploads SET state = 'completed', durable_offset = size_bytes
 WHERE id = $1 AND video_id = $2 AND state = 'active'
RETURNING object_key)sql";

// The video row cannot be missing: deleting a video deletes its uploads. deleted_at is not
// checked, as kStartProcessing does not check it: a soft-deleted video still answers its state,
// because the commit went through.
constexpr Sql kUploadState = R"sql(
SELECT uploads.state, videos.state
  FROM uploads JOIN videos ON videos.id = uploads.video_id
 WHERE uploads.id = $1 AND uploads.video_id = $2)sql";

constexpr Sql kStartProcessing = R"sql(
UPDATE videos SET state = 'processing', version = version + 1, updated_at = now()
 WHERE id = $1 AND state IN ('init', 'uploading'))sql";

constexpr Sql kQueueJob = R"sql(
INSERT INTO jobs (video_id, kind, source_key, request_id) VALUES ($1, 'transcode', $2, $3)
ON CONFLICT (video_id, kind) WHERE state IN ('queued', 'running') DO NOTHING)sql";

constexpr Sql kNotifyWorkers = "NOTIFY job_available";

constexpr Sql kAbortUpload =
    "UPDATE uploads SET state = 'aborted' WHERE id = $1 AND state = 'active'";

constexpr Sql kAbortedState = "SELECT state FROM uploads WHERE id = $1";

// No row for an upload the caller does not own, and then no lock either: the lock is taken
// only for the rows the WHERE clause lets through.
constexpr Sql kTryLock =
    "SELECT pg_try_advisory_lock($1) FROM uploads WHERE id = $2 AND owner_id = $3";
constexpr Sql kUnlock = "SELECT pg_advisory_unlock($1)";

// A batch travels as three array literals, one column each, so any batch size is one
// statement with three parameters.
constexpr Sql kRecordViews = R"sql(
INSERT INTO view_events (video_id, viewer_id, viewed_at)
SELECT video, viewer, timestamptz 'epoch' + at * interval '1 microsecond'
  FROM unnest($1::text::uuid[], $2::text::text[], $3::text::bigint[]) AS e(video, viewer, at))sql";

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

// Sizes, chunk sizes and offsets are bounded by Upload::kMaxSizeBytes, far below 2^63.
std::int64_t as_int(std::uint64_t value) noexcept {
    return static_cast<std::int64_t>(value);
}

std::int64_t micros_since_epoch(core::WallTime t) noexcept {
    return std::chrono::floor<std::chrono::microseconds>(t.time_since_epoch()).count();
}

std::optional<core::WallTime> wall_time_from_micros(std::int64_t micros) noexcept {
    // WallTime counts nanoseconds in 64 bits; beyond this the conversion would overflow.
    constexpr std::int64_t kLimit = core::WallTime::duration::max().count() / 1000;
    if (micros > kLimit || micros < -kLimit) {
        return std::nullopt;
    }
    return core::WallTime{std::chrono::microseconds{micros}};
}

std::optional<core::UploadState> parse_upload_state(std::string_view text) noexcept {
    if (text == "active") {
        return core::UploadState::Active;
    }
    if (text == "completed") {
        return core::UploadState::Completed;
    }
    if (text == "aborted") {
        return core::UploadState::Aborted;
    }
    return std::nullopt;
}

std::optional<core::VideoState> parse_video_state(std::string_view text) noexcept {
    if (text == "init") {
        return core::VideoState::Init;
    }
    if (text == "uploading") {
        return core::VideoState::Uploading;
    }
    if (text == "processing") {
        return core::VideoState::Processing;
    }
    if (text == "ready") {
        return core::VideoState::Ready;
    }
    if (text == "failed") {
        return core::VideoState::Failed;
    }
    return std::nullopt;
}

CatalogResult<StoredUpload> decode_upload(const Result& row) {
    if (row.rows() == 0) {
        return std::unexpected(CatalogError::NotFound);
    }
    const auto id = domain_at<core::UploadId>(row, 0, 0);
    const auto video = domain_at<core::VideoId>(row, 0, 1);
    const auto owner = domain_at<core::UserId>(row, 0, 2);
    const auto size = row.get(0, 3).and_then(parse_uint64);
    const auto chunk = row.get(0, 4).and_then(parse_uint64);
    const auto offset = row.get(0, 5).and_then(parse_uint64);
    const auto state = row.get(0, 6).and_then(parse_upload_state);
    const auto expires = row.get(0, 7).and_then(parse_int64).and_then(wall_time_from_micros);
    const auto backend_ref = row.get(0, 8);
    auto key = domain_at<core::StorageKey>(row, 0, 9);
    if (!id || !video || !owner || !size || !chunk || !offset || !state || !expires ||
        !backend_ref || !key) {
        return std::unexpected(CatalogError::Corrupt);
    }
    const core::UploadRecord record{.id = *id,
                                    .video_id = *video,
                                    .owner = *owner,
                                    .size_bytes = *size,
                                    .chunk_size = *chunk,
                                    .durable_offset = *offset,
                                    .state = *state,
                                    .expires_at = *expires};
    if (!core::Upload::rehydrate(record)) {
        return std::unexpected(CatalogError::Corrupt);
    }
    return StoredUpload{
        .upload = record, .backend_ref = std::string{*backend_ref}, .object_key = std::move(*key)};
}

CatalogResult<core::VideoRecord> decode_video(const Result& row) {
    if (row.rows() == 0) {
        return std::unexpected(CatalogError::NotFound);
    }
    const auto id = domain_at<core::VideoId>(row, 0, 0);
    const auto owner = domain_at<core::UserId>(row, 0, 1);
    const auto title = row.get(0, 2);
    const auto state = row.get(0, 3).and_then(parse_video_state);
    const auto version = row.get(0, 4).and_then(parse_uint64);
    if (!id || !owner || !title || !state || !version) {
        return std::unexpected(CatalogError::Corrupt);
    }
    const auto kind = row.get(0, 7);
    if (!kind) {
        return std::unexpected(CatalogError::Corrupt);
    }
    const auto visibility = core::Visibility::from_columns(*kind, row.get(0, 8));
    if (!visibility) {
        return std::unexpected(CatalogError::Corrupt);
    }
    std::optional<core::Millis> duration;
    if (const auto text = row.get(0, 6)) {
        const auto ms = parse_int64(*text);
        if (!ms) {
            return std::unexpected(CatalogError::Corrupt);
        }
        duration = core::Millis{*ms};
    }
    core::VideoRecord record{.id = *id,
                             .owner = *owner,
                             .title = std::string{*title},
                             .state = *state,
                             .version = *version,
                             .error_reason = row.get(0, 5).transform(
                                 [](std::string_view reason) { return std::string{reason}; }),
                             .duration = duration,
                             .visibility = *visibility};
    if (!core::Video::rehydrate(record)) {
        return std::unexpected(CatalogError::Corrupt);
    }
    return record;
}

template <class T> CatalogResult<T> failure(DbError e) {
    return std::unexpected(to_catalog_error(e));
}

CatalogResult<core::ports::VideoView> decode_view(const Result& row) {
    auto video = decode_video(row);
    if (!video) {
        return std::unexpected(video.error());
    }
    const auto member = row.get(0, 9).and_then(parse_bool);
    const auto granted = row.get(0, 10).and_then(parse_bool);
    if (!member || !granted) {
        return std::unexpected(CatalogError::Corrupt);
    }
    return core::ports::VideoView{.video = std::move(*video),
                                  .viewer = {.room_member = *member, .granted = *granted}};
}

// The count kGrantAccess and kRevokeAccess answer: 1 for a video that exists, 0 for none.
CatalogResult<void> decode_video_count(const Outcome& outcome) {
    if (!outcome) {
        return failure<void>(outcome.error());
    }
    if (outcome->get(0, 0).and_then(parse_uint64) != std::uint64_t{1}) {
        return std::unexpected(CatalogError::NotFound);
    }
    return {};
}

CatalogResult<core::ports::GrantPage> decode_grants(const Outcome& outcome, std::size_t limit) {
    if (!outcome) {
        return failure<core::ports::GrantPage>(outcome.error());
    }
    if (outcome->rows() == 0) {
        return std::unexpected(CatalogError::NotFound);
    }
    core::ports::GrantPage page;
    for (int i = 0; i < outcome->rows(); ++i) {
        if (!outcome->get(i, 0)) {
            // The one row of a video without grants after the cursor.
            break;
        }
        if (page.grants.size() == limit) {
            page.more = true;
            break;
        }
        const auto user = domain_at<core::UserId>(*outcome, i, 0);
        const auto at = outcome->get(i, 1).and_then(parse_int64).and_then(wall_time_from_micros);
        if (!user || !at) {
            return std::unexpected(CatalogError::Corrupt);
        }
        page.grants.push_back(core::ports::VideoGrant{.user = *user, .granted_at = *at});
    }
    return page;
}

// One statement over a video and a user id it owns, and a row limit when it has one: the
// statement goes out on a later iteration, and again after a serialization failure, long after
// the caller's UserId may be gone.
class VideoUserQuery final : public Operation {
public:
    VideoUserQuery(Sql sql, const core::VideoId& video, std::string_view user,
                   std::optional<std::int64_t> limit, Query::Done done) noexcept
        : sql_(sql), video_(video), user_(user), limit_(limit), done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        Params params;
        params.add_uuid(video_.uuid()).add_text(user_);
        if (limit_) {
            params.add_int(*limit_);
        }
        return Statement{.sql = sql_, .params = params};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        done_(std::move(outcome));
        return std::nullopt;
    }

    void abandon(DbError error) noexcept override { done_(std::unexpected(error)); }

private:
    Sql sql_;
    core::VideoId video_;
    std::string user_;
    std::optional<std::int64_t> limit_;
    Query::Done done_;
};

// The update, and when it changed nothing, a look at whether the video is the owner's: a
// refused room and a video that is not theirs answer differently.
class SetVisibility final : public Operation {
public:
    SetVisibility(const core::VideoId& video, const core::UserId& owner,
                  const core::Visibility& visibility, CatalogCallback<core::VideoRecord> done)
        : video_(video), owner_(owner.view()), kind_(visibility.kind_name()),
          room_(visibility.room_id()
                    .transform([](const core::RoomId& r) { return r.to_string(); })
                    .value_or(std::string{})),
          done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        checking_ = false;
        return Statement{
            .sql = kSetVisibility,
            .params =
                Params{}.add_uuid(video_.uuid()).add_text(owner_).add_text(kind_).add_text(room_)};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        if (!outcome) {
            done_(failure<core::VideoRecord>(outcome.error()));
            return std::nullopt;
        }
        if (!checking_) {
            if (outcome->rows() == 1) {
                done_(decode_video(*outcome));
                return std::nullopt;
            }
            checking_ = true;
            return Statement{.sql = kOwnsVideo,
                             .params = Params{}.add_uuid(video_.uuid()).add_text(owner_)};
        }
        done_(std::unexpected(outcome->rows() == 1 ? CatalogError::Forbidden
                                                   : CatalogError::NotFound));
        return std::nullopt;
    }

    void abandon(DbError error) noexcept override { done_(failure<core::VideoRecord>(error)); }

private:
    core::VideoId video_;
    std::string owner_;
    std::string kind_;
    std::string room_;
    bool checking_ = false;
    CatalogCallback<core::VideoRecord> done_;
};

class CreateUpload final : public Operation {
public:
    CreateUpload(NewUpload upload, CatalogCallback<void> done) noexcept
        : upload_(std::move(upload)), done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        const core::VideoRecord& video = upload_.video;
        const core::UploadRecord& upload = upload_.upload;
        return Statement{.sql = kCreateUpload,
                         .params = Params{}
                                       .add_uuid(video.id.uuid())
                                       .add_text(video.owner.view())
                                       .add_text(video.title)
                                       .add_uuid(upload.id.uuid())
                                       .add_text(upload.owner.view())
                                       .add_text(upload_.backend_ref)
                                       .add_text(upload_.object_key.view())
                                       .add_int(as_int(upload.chunk_size))
                                       .add_int(as_int(upload.size_bytes))
                                       .add_int(micros_since_epoch(upload.expires_at))};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        // Ids are UUIDv7 minted by the caller, so a row that already exists is this very
        // request, replayed after its reply was lost.
        if (outcome || outcome.error() == DbError::Duplicate) {
            done_({});
        } else {
            done_(failure<void>(outcome.error()));
        }
        return std::nullopt;
    }

    void abandon(DbError error) noexcept override { done_(failure<void>(error)); }

private:
    NewUpload upload_;
    CatalogCallback<void> done_;
};

// Owns the owner it binds: the statement goes out on a later iteration, and again after a
// serialization failure, long after the caller's UserId may be gone.
class TryLock final : public Operation {
public:
    TryLock(std::int64_t key, const core::UploadId& upload, const core::UserId& owner,
            Query::Done done) noexcept
        : key_(key), upload_(upload), owner_(owner), done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        return Statement{
            .sql = kTryLock,
            .params = Params{}.add_int(key_).add_uuid(upload_.uuid()).add_text(owner_.view())};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        done_(std::move(outcome));
        return std::nullopt;
    }

    void abandon(DbError error) noexcept override { done_(std::unexpected(error)); }

private:
    std::int64_t key_;
    core::UploadId upload_;
    core::UserId owner_;
    Query::Done done_;
};

class CommitUpload final : public Operation {
public:
    CommitUpload(const core::UploadId& upload, const core::VideoId& video, std::string request_id,
                 CatalogCallback<core::VideoState> done) noexcept
        : upload_(upload), video_(video), request_id_(std::move(request_id)),
          done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        step_ = Step::Begin;
        return Statement{.sql = kBegin, .params = {}};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        if (!outcome) {
            return finish(failure<core::VideoState>(outcome.error()));
        }
        switch (step_) {
        case Step::Begin:
            step_ = Step::CompleteUpload;
            return Statement{.sql = kCompleteUpload, .params = ids()};
        case Step::CompleteUpload:
            if (outcome->rows() == 0) {
                step_ = Step::ReadState;
                return Statement{.sql = kUploadState, .params = ids()};
            }
            source_key_ = outcome->get(0, 0).value_or("");
            step_ = Step::StartProcessing;
            return Statement{.sql = kStartProcessing, .params = Params{}.add_uuid(video_.uuid())};
        case Step::ReadState:
            return finish(settled(outcome->get(0, 0), outcome->get(0, 1)));
        case Step::StartProcessing:
            // The upload was active, so its video should have been init or uploading.
            if (outcome->affected() != 1) {
                return finish(std::unexpected(CatalogError::Conflict));
            }
            step_ = Step::QueueJob;
            return Statement{
                .sql = kQueueJob,
                .params =
                    Params{}.add_uuid(video_.uuid()).add_text(source_key_).add_text(request_id_)};
        case Step::QueueJob:
            step_ = Step::Notify;
            return Statement{.sql = kNotifyWorkers, .params = {}};
        case Step::Notify:
            step_ = Step::Commit;
            return Statement{.sql = kCommit, .params = {}};
        case Step::Commit:
            // The update above moved it there, inside this transaction.
            return finish(core::VideoState::Processing);
        }
        return finish(std::unexpected(CatalogError::Unavailable));
    }

    void abandon(DbError error) noexcept override { done_(failure<core::VideoState>(error)); }

private:
    enum class Step : std::uint8_t {
        Begin,
        CompleteUpload,
        ReadState,
        StartProcessing,
        QueueJob,
        Notify,
        Commit
    };

    // The upload was not active. Completed means an earlier commit went through, and this one
    // succeeds without doing anything (the pool rolls back the empty transaction), answering
    // the video's state as it stands.
    static CatalogResult<core::VideoState> settled(std::optional<std::string_view> upload,
                                                   std::optional<std::string_view> video) noexcept {
        if (!upload) {
            return std::unexpected(CatalogError::NotFound);
        }
        if (*upload != "completed") {
            return std::unexpected(CatalogError::Conflict);
        }
        // The column's enum type admits no other value; the commit went through whatever it says.
        return video.and_then(parse_video_state).value_or(core::VideoState::Processing);
    }

    [[nodiscard]] Params ids() const noexcept {
        return Params{}.add_uuid(upload_.uuid()).add_uuid(video_.uuid());
    }

    std::optional<Statement> finish(CatalogResult<core::VideoState> result) noexcept {
        done_(result);
        return std::nullopt;
    }

    core::UploadId upload_;
    core::VideoId video_;
    std::string request_id_;
    std::string source_key_;
    Step step_ = Step::Begin;
    CatalogCallback<core::VideoState> done_;
};

class AbortUpload final : public Operation {
public:
    AbortUpload(const core::UploadId& upload, CatalogCallback<void> done) noexcept
        : upload_(upload), done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        reading_state_ = false;
        return Statement{.sql = kAbortUpload, .params = Params{}.add_uuid(upload_.uuid())};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        if (!outcome) {
            done_(failure<void>(outcome.error()));
            return std::nullopt;
        }
        if (!reading_state_) {
            if (outcome->affected() == 1) {
                done_({});
                return std::nullopt;
            }
            reading_state_ = true;
            return Statement{.sql = kAbortedState, .params = Params{}.add_uuid(upload_.uuid())};
        }
        const auto state = outcome->get(0, 0);
        if (!state) {
            done_(std::unexpected(CatalogError::NotFound));
        } else if (*state == "aborted") {
            done_({});
        } else {
            done_(std::unexpected(CatalogError::Conflict));
        }
        return std::nullopt;
    }

    void abandon(DbError error) noexcept override { done_(failure<void>(error)); }

private:
    core::UploadId upload_;
    bool reading_state_ = false;
    CatalogCallback<void> done_;
};

// Owns the array literals it binds, since the statement may go out again after a
// serialization failure.
class RecordViews final : public Operation {
public:
    RecordViews(std::span<const core::ports::ViewEvent> batch, CatalogCallback<void> done)
        : done_(std::move(done)) {
        // Viewers are quoted: an unquoted NULL, in any case, is SQL NULL inside an array
        // literal, and "null" is a valid subject. Quoting needs no escapes, since UserId allows
        // only [A-Za-z0-9._:@|+-].
        for (const core::ports::ViewEvent& e : batch) {
            separate();
            videos_ += e.video.to_string();
            viewers_.append(1, '"').append(e.viewer.view()).append(1, '"');
            times_ += std::to_string(micros_since_epoch(e.at));
        }
        videos_ += '}';
        viewers_ += '}';
        times_ += '}';
    }

    [[nodiscard]] Statement start() noexcept override {
        return Statement{.sql = kRecordViews,
                         .params = Params{}.add_text(videos_).add_text(viewers_).add_text(times_)};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        done_(outcome ? CatalogResult<void>{} : failure<void>(outcome.error()));
        return std::nullopt;
    }

    void abandon(DbError error) noexcept override { done_(failure<void>(error)); }

private:
    void separate() {
        const char c = videos_.empty() ? '{' : ',';
        videos_ += c;
        viewers_ += c;
        times_ += c;
    }

    std::string videos_;
    std::string viewers_;
    std::string times_;
    CatalogCallback<void> done_;
};

} // namespace

class PgUploadCatalog::Impl {
public:
    Impl(net::IReactor& reactor, std::unique_ptr<Pool> main, std::unique_ptr<Pool> locks)
        : main_(std::move(main)), locks_(std::move(locks)), deferred_(reactor) {}

    Pool& main_pool() noexcept { return *main_; }

    void settle(CatalogCallback<void> done) {
        deferred_.post([done = std::move(done)]() mutable noexcept { done({}); });
    }

    template <class T> void refuse(CatalogCallback<T> done, CatalogError error) {
        deferred_.post(
            [done = std::move(done), error]() mutable noexcept { done(std::unexpected(error)); });
    }

    void claim(const core::UploadId& id, const core::UserId& owner,
               CatalogCallback<ClaimedUpload> done) {
        if (const auto it = claims_.find(id); it != claims_.end()) {
            // Advisory locks are reentrant within a session, so the server would grant this
            // process a second claim; only this map can refuse it.
            if (it->second.state != ClaimState::Held || it->second.session == live_session()) {
                refuse(std::move(done), CatalogError::Conflict);
                return;
            }
            claims_.erase(it);
        }
        const std::int64_t key = lock_key(id);
        // A grant of its own: a holder of an earlier grant of this upload, lost with its
        // session, must not be able to release this one.
        const ClaimToken token{++last_token_};
        claims_.emplace(
            id, Claim{.key = key, .session = 0, .token = token, .state = ClaimState::Locking});
        locks_->submit(std::make_unique<TryLock>(
            key, id, owner,
            [this, id, token, done = std::move(done)](Outcome outcome) mutable noexcept {
                locked(id, token, std::move(outcome), std::move(done));
            }));
    }

    void release(const core::UploadId& id, ClaimToken token) noexcept {
        const auto it = claims_.find(id);
        if (it == claims_.end() || it->second.state != ClaimState::Held ||
            it->second.token != token) {
            return;
        }
        const Claim claim = it->second;
        claims_.erase(it);
        // A lock from a session that has ended is gone already.
        if (claim.session == live_session()) {
            unlock(claim.key);
        }
    }

    // `token` names the upload's current grant, held on the live session.
    [[nodiscard]] bool holds(const core::UploadId& id, ClaimToken token) const noexcept {
        const auto it = claims_.find(id);
        return it != claims_.end() && it->second.state == ClaimState::Held &&
               it->second.token == token && it->second.session == live_session();
    }

private:
    enum class ClaimState : std::uint8_t { Locking, Loading, Held };

    struct Claim {
        std::int64_t key = 0;
        std::uint64_t session = 0;
        ClaimToken token;
        ClaimState state = ClaimState::Locking;
    };

    // The grant a lock or load answers for, or end() when it is no longer in the map.
    [[nodiscard]] std::unordered_map<core::UploadId, Claim>::iterator
    grant(const core::UploadId& id, ClaimToken token) noexcept {
        const auto it = claims_.find(id);
        return it != claims_.end() && it->second.token == token ? it : claims_.end();
    }

    [[nodiscard]] std::uint64_t live_session() const noexcept { return locks_->sessions_lost(); }

    void locked(const core::UploadId& id, ClaimToken token, Outcome outcome,
                CatalogCallback<ClaimedUpload> done) {
        const auto it = grant(id, token);
        if (it == claims_.end()) {
            done(std::unexpected(CatalogError::Unavailable));
            return;
        }
        if (!outcome) {
            claims_.erase(it);
            done(failure<ClaimedUpload>(outcome.error()));
            return;
        }
        if (outcome->rows() == 0) {
            claims_.erase(it);
            done(std::unexpected(CatalogError::NotFound));
            return;
        }
        if (outcome->get(0, 0).and_then(parse_bool) != true) {
            claims_.erase(it);
            done(std::unexpected(CatalogError::Conflict));
            return;
        }
        // The session that answered is the live one: a lost session abandons its queries.
        it->second.session = live_session();
        it->second.state = ClaimState::Loading;
        main_->submit(std::make_unique<Query>(
            Statement{.sql = kFindUpload, .params = Params{}.add_uuid(id.uuid())},
            [this, id, token, done = std::move(done)](Outcome row) mutable noexcept {
                loaded(id, token, std::move(row), std::move(done));
            }));
    }

    void loaded(const core::UploadId& id, ClaimToken token, Outcome outcome,
                CatalogCallback<ClaimedUpload> done) {
        const auto it = grant(id, token);
        if (it == claims_.end()) {
            done(std::unexpected(CatalogError::Unavailable));
            return;
        }
        const Claim claim = it->second;
        CatalogResult<StoredUpload> upload =
            outcome ? decode_upload(*outcome) : failure<StoredUpload>(outcome.error());
        if (claim.session != live_session()) {
            claims_.erase(it);
            done(std::unexpected(CatalogError::Unavailable));
            return;
        }
        if (!upload) {
            claims_.erase(it);
            unlock(claim.key);
            done(std::unexpected(upload.error()));
            return;
        }
        it->second.state = ClaimState::Held;
        done(ClaimedUpload{.stored = std::move(*upload), .token = token});
    }

    void unlock(std::int64_t key) {
        locks_->submit(
            std::make_unique<Query>(Statement{.sql = kUnlock, .params = Params{}.add_int(key)},
                                    [](Outcome /*outcome*/) noexcept {}));
    }

    std::unique_ptr<Pool> main_;
    // One session holds every claim this process has.
    std::unique_ptr<Pool> locks_;
    DeferredCalls deferred_;
    std::unordered_map<core::UploadId, Claim> claims_;
    std::uint64_t last_token_ = 0;
};

std::expected<std::unique_ptr<PgUploadCatalog>, std::string>
PgUploadCatalog::create(net::IReactor& reactor, net::OffloadPool& offload,
                        const CatalogConfig& config) {
    auto main =
        Pool::create(reactor, offload,
                     PoolConfig{.conninfo = config.conninfo,
                                .application_name = "ulw-catalog",
                                .connections = std::clamp<std::size_t>(config.connections, 8, 16),
                                .connect_timeout = config.connect_timeout,
                                .request_timeout = config.request_timeout});
    if (!main) {
        return std::unexpected(std::move(main.error()));
    }
    auto locks = Pool::create(reactor, offload,
                              PoolConfig{.conninfo = config.conninfo,
                                         .application_name = "ulw-claims",
                                         .connections = 1,
                                         .connect_timeout = config.connect_timeout,
                                         .request_timeout = config.request_timeout});
    if (!locks) {
        return std::unexpected(std::move(locks.error()));
    }
    return std::make_unique<PgUploadCatalog>(
        Token{}, std::make_unique<Impl>(reactor, std::move(*main), std::move(*locks)));
}

PgUploadCatalog::PgUploadCatalog(Token /*token*/, std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

PgUploadCatalog::~PgUploadCatalog() = default;

void PgUploadCatalog::create_upload(NewUpload upload, CatalogCallback<void> done) {
    impl_->main_pool().submit(std::make_unique<CreateUpload>(std::move(upload), std::move(done)));
}

void PgUploadCatalog::find_upload(const core::UploadId& id, CatalogCallback<StoredUpload> done) {
    impl_->main_pool().submit(std::make_unique<Query>(
        Statement{.sql = kFindUpload, .params = Params{}.add_uuid(id.uuid())},
        [done = std::move(done)](Outcome outcome) mutable noexcept {
            done(outcome ? decode_upload(*outcome) : failure<StoredUpload>(outcome.error()));
        }));
}

void PgUploadCatalog::claim_upload(const core::UploadId& id, const core::UserId& owner,
                                   CatalogCallback<ClaimedUpload> done) {
    impl_->claim(id, owner, std::move(done));
}

void PgUploadCatalog::release_upload(const core::UploadId& id, ClaimToken token) noexcept {
    impl_->release(id, token);
}

void PgUploadCatalog::record_progress(const core::UploadId& id, ClaimToken token,
                                      const core::VideoId& video, std::uint64_t durable_offset,
                                      CatalogCallback<void> done) {
    if (!impl_->holds(id, token)) {
        impl_->refuse(std::move(done), CatalogError::Conflict);
        return;
    }
    impl_->main_pool().submit(std::make_unique<Query>(
        Statement{.sql = kRecordProgress,
                  .params = Params{}
                                .add_uuid(id.uuid())
                                .add_int(as_int(durable_offset))
                                .add_uuid(video.uuid())},
        [done = std::move(done)](Outcome outcome) mutable noexcept {
            if (!outcome) {
                done(failure<void>(outcome.error()));
                return;
            }
            // No row: the upload is no longer active.
            if (outcome->get(0, 0).and_then(parse_uint64) != std::uint64_t{1}) {
                done(std::unexpected(CatalogError::Conflict));
                return;
            }
            done({});
        }));
}

void PgUploadCatalog::commit_upload(const core::UploadId& id, const core::VideoId& video,
                                    const std::string& request_id,
                                    CatalogCallback<core::VideoState> done) {
    impl_->main_pool().submit(
        std::make_unique<CommitUpload>(id, video, request_id, std::move(done)));
}

void PgUploadCatalog::abort_upload(const core::UploadId& id, CatalogCallback<void> done) {
    impl_->main_pool().submit(std::make_unique<AbortUpload>(id, std::move(done)));
}

void PgUploadCatalog::find_video_for(const core::VideoId& id, const core::UserId& viewer,
                                     CatalogCallback<core::ports::VideoView> done) {
    impl_->main_pool().submit(std::make_unique<VideoUserQuery>(
        kFindVideoFor, id, viewer.view(), std::nullopt,
        [done = std::move(done)](Outcome outcome) mutable noexcept {
            done(outcome ? decode_view(*outcome)
                         : failure<core::ports::VideoView>(outcome.error()));
        }));
}

void PgUploadCatalog::set_visibility(const core::VideoId& id, const core::UserId& owner,
                                     const core::Visibility& visibility,
                                     CatalogCallback<core::VideoRecord> done) {
    impl_->main_pool().submit(
        std::make_unique<SetVisibility>(id, owner, visibility, std::move(done)));
}

void PgUploadCatalog::grant_access(const core::VideoId& id, const core::UserId& user,
                                   CatalogCallback<void> done) {
    impl_->main_pool().submit(std::make_unique<VideoUserQuery>(
        kGrantAccess, id, user.view(), std::nullopt,
        [done = std::move(done)](Outcome outcome) mutable noexcept {
            // The foreign key refused the row: the video was deleted between the statement's
            // look and its insert, which is a video that does not exist.
            if (!outcome && outcome.error() == DbError::Constraint) {
                done(std::unexpected(CatalogError::NotFound));
                return;
            }
            done(decode_video_count(outcome));
        }));
}

void PgUploadCatalog::revoke_access(const core::VideoId& id, const core::UserId& user,
                                    CatalogCallback<void> done) {
    impl_->main_pool().submit(std::make_unique<VideoUserQuery>(
        kRevokeAccess, id, user.view(), std::nullopt,
        [done = std::move(done)](Outcome outcome) mutable noexcept {
            done(decode_video_count(outcome));
        }));
}

void PgUploadCatalog::list_grants(const core::VideoId& id, std::optional<core::UserId> after,
                                  std::size_t limit, CatalogCallback<core::ports::GrantPage> done) {
    // One more than asked, to tell whether more follow. '' sorts before every user id.
    const std::string_view cursor = after ? after->view() : std::string_view{};
    impl_->main_pool().submit(std::make_unique<VideoUserQuery>(
        kListGrants, id, cursor, as_int(limit) + 1,
        [done = std::move(done), limit](Outcome outcome) mutable noexcept {
            done(decode_grants(outcome, limit));
        }));
}

void PgUploadCatalog::record_views(std::vector<core::ports::ViewEvent> batch,
                                   CatalogCallback<void> done) {
    if (batch.empty()) {
        impl_->settle(std::move(done));
        return;
    }
    impl_->main_pool().submit(std::make_unique<RecordViews>(batch, std::move(done)));
}

} // namespace infra::postgres
