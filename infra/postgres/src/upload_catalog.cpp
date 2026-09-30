#include "infra/postgres/upload_catalog.hpp"

#include "deferred_calls.hpp"
#include "lock_key.hpp"
#include "operation.hpp"
#include "pool.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace infra::postgres {

namespace {

using core::ports::CatalogCallback;
using core::ports::CatalogError;
using core::ports::CatalogResult;
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

constexpr Sql kFindVideo = R"sql(
SELECT id, owner_id, title, state, version, error_reason, duration_ms
  FROM videos WHERE id = $1 AND deleted_at IS NULL)sql";

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

constexpr Sql kUploadState = "SELECT state FROM uploads WHERE id = $1 AND video_id = $2";

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
                             .duration = duration};
    if (!core::Video::rehydrate(record)) {
        return std::unexpected(CatalogError::Corrupt);
    }
    return record;
}

template <class T> CatalogResult<T> failure(DbError e) {
    return std::unexpected(to_catalog_error(e));
}

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
                 CatalogCallback<void> done) noexcept
        : upload_(upload), video_(video), request_id_(std::move(request_id)),
          done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        step_ = Step::Begin;
        return Statement{.sql = kBegin, .params = {}};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        if (!outcome) {
            return finish(failure<void>(outcome.error()));
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
            return finish(settled(outcome->get(0, 0)));
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
            return finish({});
        }
        return finish(std::unexpected(CatalogError::Unavailable));
    }

    void abandon(DbError error) noexcept override { done_(failure<void>(error)); }

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
    // succeeds without doing anything (the pool rolls back the empty transaction).
    static CatalogResult<void> settled(std::optional<std::string_view> state) noexcept {
        if (!state) {
            return std::unexpected(CatalogError::NotFound);
        }
        if (*state == "completed") {
            return {};
        }
        return std::unexpected(CatalogError::Conflict);
    }

    [[nodiscard]] Params ids() const noexcept {
        return Params{}.add_uuid(upload_.uuid()).add_uuid(video_.uuid());
    }

    std::optional<Statement> finish(CatalogResult<void> result) noexcept {
        done_(result);
        return std::nullopt;
    }

    core::UploadId upload_;
    core::VideoId video_;
    std::string request_id_;
    std::string source_key_;
    Step step_ = Step::Begin;
    CatalogCallback<void> done_;
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
               CatalogCallback<StoredUpload> done) {
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
        claims_.emplace(id, Claim{.key = key, .session = 0, .state = ClaimState::Locking});
        locks_->submit(std::make_unique<TryLock>(
            key, id, owner, [this, id, done = std::move(done)](Outcome outcome) mutable noexcept {
                locked(id, std::move(outcome), std::move(done));
            }));
    }

    void release(const core::UploadId& id) noexcept {
        const auto it = claims_.find(id);
        if (it == claims_.end() || it->second.state != ClaimState::Held) {
            return;
        }
        const Claim claim = it->second;
        claims_.erase(it);
        // A lock from a session that has ended is gone already.
        if (claim.session == live_session()) {
            unlock(claim.key);
        }
    }

    [[nodiscard]] bool holds(const core::UploadId& id) const noexcept {
        const auto it = claims_.find(id);
        return it != claims_.end() && it->second.state == ClaimState::Held &&
               it->second.session == live_session();
    }

private:
    enum class ClaimState : std::uint8_t { Locking, Loading, Held };

    struct Claim {
        std::int64_t key = 0;
        std::uint64_t session = 0;
        ClaimState state = ClaimState::Locking;
    };

    [[nodiscard]] std::uint64_t live_session() const noexcept { return locks_->sessions_lost(); }

    void locked(const core::UploadId& id, Outcome outcome, CatalogCallback<StoredUpload> done) {
        const auto it = claims_.find(id);
        if (it == claims_.end()) {
            done(std::unexpected(CatalogError::Unavailable));
            return;
        }
        if (!outcome) {
            claims_.erase(it);
            done(failure<StoredUpload>(outcome.error()));
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
            [this, id, done = std::move(done)](Outcome row) mutable noexcept {
                loaded(id, std::move(row), std::move(done));
            }));
    }

    void loaded(const core::UploadId& id, Outcome outcome, CatalogCallback<StoredUpload> done) {
        const auto it = claims_.find(id);
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
            done(std::move(upload));
            return;
        }
        it->second.state = ClaimState::Held;
        done(std::move(upload));
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
                                   CatalogCallback<StoredUpload> done) {
    impl_->claim(id, owner, std::move(done));
}

void PgUploadCatalog::release_upload(const core::UploadId& id) noexcept {
    impl_->release(id);
}

void PgUploadCatalog::record_progress(const core::UploadId& id, const core::VideoId& video,
                                      std::uint64_t durable_offset, CatalogCallback<void> done) {
    if (!impl_->holds(id)) {
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
                                    const std::string& request_id, CatalogCallback<void> done) {
    impl_->main_pool().submit(
        std::make_unique<CommitUpload>(id, video, request_id, std::move(done)));
}

void PgUploadCatalog::abort_upload(const core::UploadId& id, CatalogCallback<void> done) {
    impl_->main_pool().submit(std::make_unique<AbortUpload>(id, std::move(done)));
}

void PgUploadCatalog::find_video(const core::VideoId& id, CatalogCallback<core::VideoRecord> done) {
    impl_->main_pool().submit(std::make_unique<Query>(
        Statement{.sql = kFindVideo, .params = Params{}.add_uuid(id.uuid())},
        [done = std::move(done)](Outcome outcome) mutable noexcept {
            done(outcome ? decode_video(*outcome) : failure<core::VideoRecord>(outcome.error()));
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
