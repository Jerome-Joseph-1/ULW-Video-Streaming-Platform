#include "infra/postgres/live_streams.hpp"

#include "deferred_calls.hpp"
#include "message_sql.hpp"
#include "operation.hpp"
#include "pool.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace infra::postgres {

namespace {

using core::ports::CreatedLiveStream;
using core::ports::EndedLiveStream;
using core::ports::LiveCallback;
using core::ports::LiveEnd;
using core::ports::LiveResult;
using core::ports::LiveState;
using core::ports::LiveStoreError;
using core::ports::LiveStream;

// Every statement answers the same columns, in the same order: timestamps as integer
// microseconds since the epoch (exact, and free of DateStyle and TimeZone), and last the
// recording's video from live_recordings, whose key is the stream id's text.
// Every stream is created under this one transaction-scoped advisory lock, taken in a statement
// of its own before the insert: the insert's count of unfinished and recent streams then reads a
// snapshot taken after the previous creator committed, so concurrent creates cannot pass the
// caps together. A lock in the insert's own statement would come after its snapshot.
// 0x756c77_6c697665: "ulw" "live".
constexpr Sql kLockCreates = "SELECT pg_advisory_xact_lock(128859950462053)";

// The row, unless the owner has an unfinished stream already, or `max` streams are unfinished,
// or the owner created $6 streams in the hour before $4.
constexpr Sql kCreate = R"sql(
INSERT INTO live_streams AS s (id, owner_id, srt_passphrase, created_at)
SELECT $1, $2, $3, timestamptz 'epoch' + $4 * interval '1 microsecond'
 WHERE (SELECT count(*) FROM live_streams WHERE state <> 'ended') < $5
   AND (SELECT count(*) FROM live_streams
         WHERE owner_id = $2
           AND created_at > timestamptz 'epoch' + $4 * interval '1 microsecond'
                            - interval '1 hour') < $6
ON CONFLICT (owner_id) WHERE state <> 'ended' DO NOTHING
RETURNING s.id, s.owner_id, s.state, (extract(epoch FROM s.created_at) * 1000000)::bigint,
       (extract(epoch FROM s.live_at) * 1000000)::bigint,
       (extract(epoch FROM s.ended_at) * 1000000)::bigint, s.end_reason, s.srt_passphrase,
       NULL::uuid)sql";

// Why a create stored nothing when the owner had no unfinished stream: their hourly count.
constexpr Sql kRecentByOwner = R"sql(
SELECT count(*) FROM live_streams
 WHERE owner_id = $1
   AND created_at > timestamptz 'epoch' + $2 * interval '1 microsecond' - interval '1 hour')sql";

// The stream's live chat (ADR-0070): its room, then opened with the chat store's own guarded
// statement (message_sql::kRecordLive), whatever starts a stream opens its chat.
constexpr Sql kChatRoom = "SELECT live_chat_room($1::uuid::text)";

// Closes the stream's live chat when the stream ends: no join is admitted to it again
// (migration 0012, message_sql::kAdmits).
constexpr Sql kCloseChat = R"sql(
UPDATE chat_rooms SET closed_at = now()
 WHERE room_id = live_chat_room($1::uuid::text) AND kind = 'stream_live_chat'
   AND closed_at IS NULL)sql";

constexpr Sql kCurrent = R"sql(
SELECT s.id, s.owner_id, s.state, (extract(epoch FROM s.created_at) * 1000000)::bigint,
       (extract(epoch FROM s.live_at) * 1000000)::bigint,
       (extract(epoch FROM s.ended_at) * 1000000)::bigint, s.end_reason, s.srt_passphrase,
       (SELECT r.video_id FROM live_recordings r WHERE r.stream_id = s.id::text)
  FROM live_streams s WHERE s.owner_id = $1 AND s.state <> 'ended')sql";

constexpr Sql kFind = R"sql(
SELECT s.id, s.owner_id, s.state, (extract(epoch FROM s.created_at) * 1000000)::bigint,
       (extract(epoch FROM s.live_at) * 1000000)::bigint,
       (extract(epoch FROM s.ended_at) * 1000000)::bigint, s.end_reason, s.srt_passphrase,
       (SELECT r.video_id FROM live_recordings r WHERE r.stream_id = s.id::text)
  FROM live_streams s WHERE s.id = $1)sql";

constexpr Sql kMarkLive = R"sql(
UPDATE live_streams s
   SET state = 'live',
       live_at = COALESCE(s.live_at, timestamptz 'epoch' + $2 * interval '1 microsecond')
 WHERE s.id = $1 AND s.state <> 'ended'
RETURNING s.id, s.owner_id, s.state, (extract(epoch FROM s.created_at) * 1000000)::bigint,
       (extract(epoch FROM s.live_at) * 1000000)::bigint,
       (extract(epoch FROM s.ended_at) * 1000000)::bigint, s.end_reason, s.srt_passphrase,
       (SELECT r.video_id FROM live_recordings r WHERE r.stream_id = s.id::text))sql";

constexpr Sql kEnd = R"sql(
UPDATE live_streams s
   SET state = 'ended', end_reason = $2,
       ended_at = timestamptz 'epoch' + $3 * interval '1 microsecond'
 WHERE s.id = $1 AND s.state <> 'ended'
RETURNING s.id, s.owner_id, s.state, (extract(epoch FROM s.created_at) * 1000000)::bigint,
       (extract(epoch FROM s.live_at) * 1000000)::bigint,
       (extract(epoch FROM s.ended_at) * 1000000)::bigint, s.end_reason, s.srt_passphrase,
       (SELECT r.video_id FROM live_recordings r WHERE r.stream_id = s.id::text))sql";

constexpr Sql kUnfinished = R"sql(
SELECT s.id, s.owner_id, s.state, (extract(epoch FROM s.created_at) * 1000000)::bigint,
       (extract(epoch FROM s.live_at) * 1000000)::bigint,
       (extract(epoch FROM s.ended_at) * 1000000)::bigint, s.end_reason, s.srt_passphrase,
       (SELECT r.video_id FROM live_recordings r WHERE r.stream_id = s.id::text)
  FROM live_streams s WHERE s.state <> 'ended'
 ORDER BY s.created_at, s.id LIMIT $1)sql";

LiveStoreError to_live_error(DbError e) noexcept {
    switch (e) {
    case DbError::Duplicate:
    case DbError::Constraint:
    case DbError::Retry:
    case DbError::ConnectionLost:
    case DbError::Timeout:
    case DbError::LockTimeout:
    case DbError::Rejected:
        return LiveStoreError::Unavailable;
    }
    return LiveStoreError::Unavailable;
}

std::int64_t micros_since_epoch(core::WallTime t) noexcept {
    return std::chrono::floor<std::chrono::microseconds>(t.time_since_epoch()).count();
}

std::optional<core::WallTime> wall_time_from_micros(std::int64_t micros) noexcept {
    constexpr std::int64_t kLimit = core::WallTime::duration::max().count() / 1000;
    if (micros > kLimit || micros < -kLimit) {
        return std::nullopt;
    }
    return core::WallTime{std::chrono::microseconds{micros}};
}

std::optional<LiveState> parse_state(std::string_view text) noexcept {
    if (text == "starting") {
        return LiveState::Starting;
    }
    if (text == "live") {
        return LiveState::Live;
    }
    if (text == "ended") {
        return LiveState::Ended;
    }
    return std::nullopt;
}

std::optional<LiveEnd> parse_end(std::string_view text) noexcept {
    for (const LiveEnd e : {LiveEnd::Owner, LiveEnd::Finished, LiveEnd::Failed, LiveEnd::Timeout,
                            LiveEnd::PublisherLeft}) {
        if (text == core::ports::to_string(e)) {
            return e;
        }
    }
    return std::nullopt;
}

// A nullable timestamp column: absent is fine, unreadable is not.
std::optional<std::optional<core::WallTime>> time_at(const Result& rows, int row, int column) {
    const auto text = rows.get(row, column);
    if (!text) {
        return std::optional<core::WallTime>{};
    }
    const auto at = parse_int64(*text).and_then(wall_time_from_micros);
    if (!at) {
        return std::nullopt;
    }
    return at;
}

LiveResult<LiveStream> decode_row(const Result& rows, int row) {
    const auto id = domain_at<core::LiveStreamId>(rows, row, 0);
    const auto owner = domain_at<core::UserId>(rows, row, 1);
    const auto state = rows.get(row, 2).and_then(parse_state);
    const auto created = rows.get(row, 3).and_then(parse_int64).and_then(wall_time_from_micros);
    const auto live_at = time_at(rows, row, 4);
    const auto ended_at = time_at(rows, row, 5);
    const auto reason_text = rows.get(row, 6);
    const auto reason = reason_text ? parse_end(*reason_text) : std::nullopt;
    const auto passphrase = rows.get(row, 7);
    const auto recording_text = rows.get(row, 8);
    const auto recording = recording_text ? domain_at<core::VideoId>(rows, row, 8) : std::nullopt;
    if (!id || !owner || !state || !created || !live_at || !ended_at || (reason_text && !reason) ||
        !passphrase || (recording_text && !recording) ||
        (*state == LiveState::Ended) != ended_at->has_value()) {
        return std::unexpected(LiveStoreError::Corrupt);
    }
    return LiveStream{.id = *id,
                      .owner = *owner,
                      .state = *state,
                      .passphrase = std::string(*passphrase),
                      .created_at = *created,
                      .live_at = *live_at,
                      .ended_at = *ended_at,
                      .ended_by = reason,
                      .recording = recording};
}

LiveResult<std::vector<LiveStream>> decode_rows(const Result& rows) {
    std::vector<LiveStream> out;
    out.reserve(static_cast<std::size_t>(rows.rows()));
    for (int i = 0; i < rows.rows(); ++i) {
        auto stream = decode_row(rows, i);
        if (!stream) {
            return std::unexpected(stream.error());
        }
        out.push_back(std::move(*stream));
    }
    return out;
}

// A write that may touch no row, followed then by a read of the row as it stands: the answer is
// the written row, or the read one and false.
template <class Answer> class WriteThenRead final : public Operation {
public:
    using Decide = std::move_only_function<LiveResult<Answer>(LiveResult<LiveStream>, bool wrote)>;

    WriteThenRead(Statement write, Statement read, Decide decide,
                  LiveCallback<Answer> done) noexcept
        : write_(write), read_(read), decide_(std::move(decide)), done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        reading_ = false;
        return write_;
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        if (!outcome) {
            done_(std::unexpected(to_live_error(outcome.error())));
            return std::nullopt;
        }
        if (outcome->rows() == 0 && !reading_) {
            reading_ = true;
            return read_;
        }
        LiveResult<LiveStream> row =
            outcome->rows() == 0 ? LiveResult<LiveStream>(std::unexpected(LiveStoreError::NotFound))
                                 : decode_row(*outcome, 0);
        done_(decide_(std::move(row), !reading_));
        return std::nullopt;
    }

    void abandon(DbError error) noexcept override { done_(std::unexpected(to_live_error(error))); }

private:
    Statement write_;
    Statement read_;
    Decide decide_;
    LiveCallback<Answer> done_;
    bool reading_ = false;
};

constexpr Sql kBegin = "BEGIN";
constexpr Sql kCommit = "COMMIT";

// A stream's creation, in one transaction: the lock that serialises creates, the insert, and
// then either the stream's chat opened, or the owner's unfinished stream read back, or the
// reason nothing was stored. Its answer is given once the commit has succeeded.
class CreateStream final : public Operation {
public:
    CreateStream(core::ports::NewLiveStream stream, core::ports::LiveLimits limits,
                 LiveCallback<CreatedLiveStream> done) noexcept
        : stream_(std::move(stream)), limits_(limits), done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        stage_ = Stage::Begin;
        answer_ = std::unexpected(LiveStoreError::Unavailable);
        return {.sql = kBegin, .params = {}};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        if (!outcome) {
            done_(std::unexpected(to_live_error(outcome.error())));
            return std::nullopt;
        }
        switch (stage_) {
        case Stage::Begin:
            stage_ = Stage::Lock;
            return Statement{.sql = kLockCreates, .params = {}};
        case Stage::Lock:
            stage_ = Stage::Insert;
            return Statement{.sql = kCreate,
                             .params = Params{}
                                           .add_uuid(stream_.id.uuid())
                                           .add_text(stream_.owner.view())
                                           .add_text(stream_.passphrase)
                                           .add_int(micros_since_epoch(stream_.at))
                                           .add_int(limits_.max_unfinished)
                                           .add_int(limits_.per_owner_per_hour)};
        case Stage::Insert:
            return inserted(*outcome);
        case Stage::ChatRoom: {
            const auto room = outcome->get(0, 0).and_then(core::Uuid::parse);
            if (!room) {
                done_(std::unexpected(LiveStoreError::Corrupt));
                return std::nullopt;
            }
            room_ = *room;
            stage_ = Stage::OpenChat;
            return Statement{.sql = message_sql::kRecordLive, .params = Params{}.add_uuid(*room_)};
        }
        case Stage::OpenChat:
            return commit();
        case Stage::Current:
            if (outcome->rows() > 0) {
                answer_ = decode_row(*outcome, 0).transform([](LiveStream s) {
                    return CreatedLiveStream{.stream = std::move(s), .created = false};
                });
                return commit();
            }
            stage_ = Stage::Recent;
            return Statement{.sql = kRecentByOwner,
                             .params = Params{}
                                           .add_text(stream_.owner.view())
                                           .add_int(micros_since_epoch(stream_.at))};
        case Stage::Recent: {
            const auto recent = outcome->get(0, 0).and_then(parse_int64);
            answer_ = std::unexpected(recent && *recent >= limits_.per_owner_per_hour
                                          ? LiveStoreError::TooMany
                                          : LiveStoreError::Full);
            return commit();
        }
        case Stage::Commit:
            done_(std::move(answer_));
            return std::nullopt;
        }
        return std::nullopt;
    }

    void abandon(DbError error) noexcept override { done_(std::unexpected(to_live_error(error))); }

private:
    enum class Stage : std::uint8_t {
        Begin,
        Lock,
        Insert,
        ChatRoom,
        OpenChat,
        Current,
        Recent,
        Commit
    };

    [[nodiscard]] std::optional<Statement> inserted(const Result& rows) noexcept {
        if (rows.rows() == 0) {
            stage_ = Stage::Current;
            return Statement{.sql = kCurrent, .params = Params{}.add_text(stream_.owner.view())};
        }
        answer_ = decode_row(rows, 0).transform([](LiveStream s) {
            return CreatedLiveStream{.stream = std::move(s), .created = true};
        });
        if (!answer_) {
            done_(std::unexpected(answer_.error()));
            return std::nullopt;
        }
        stage_ = Stage::ChatRoom;
        return Statement{.sql = kChatRoom, .params = Params{}.add_uuid(stream_.id.uuid())};
    }

    [[nodiscard]] Statement commit() noexcept {
        stage_ = Stage::Commit;
        return {.sql = kCommit, .params = {}};
    }

    core::ports::NewLiveStream stream_;
    core::ports::LiveLimits limits_;
    LiveCallback<CreatedLiveStream> done_;
    LiveResult<CreatedLiveStream> answer_ = std::unexpected(LiveStoreError::Unavailable);
    std::optional<core::Uuid> room_;
    Stage stage_ = Stage::Begin;
};

// A stream's end, with its live chat closed in the same transaction, or the stream read back
// as it stands when it had ended already.
class EndStream final : public Operation {
public:
    EndStream(const core::LiveStreamId& id, LiveEnd reason, core::WallTime at,
              LiveCallback<EndedLiveStream> done) noexcept
        : id_(id), reason_(reason), at_(at), done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        stage_ = Stage::Begin;
        return {.sql = kBegin, .params = {}};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        if (!outcome) {
            done_(std::unexpected(to_live_error(outcome.error())));
            return std::nullopt;
        }
        switch (stage_) {
        case Stage::Begin:
            stage_ = Stage::End;
            return Statement{.sql = kEnd,
                             .params = Params{}
                                           .add_uuid(id_.uuid())
                                           .add_text(core::ports::to_string(reason_))
                                           .add_int(micros_since_epoch(at_))};
        case Stage::End:
            if (outcome->rows() == 0) {
                stage_ = Stage::Read;
                return Statement{.sql = kFind, .params = Params{}.add_uuid(id_.uuid())};
            }
            answer_ = decode_row(*outcome, 0).transform([](LiveStream s) {
                return EndedLiveStream{.stream = std::move(s), .ended = true};
            });
            stage_ = Stage::CloseChat;
            return Statement{.sql = kCloseChat, .params = Params{}.add_uuid(id_.uuid())};
        case Stage::Read:
            answer_ = outcome->rows() == 0
                          ? LiveResult<EndedLiveStream>(std::unexpected(LiveStoreError::NotFound))
                          : decode_row(*outcome, 0).transform([](LiveStream s) {
                                return EndedLiveStream{.stream = std::move(s), .ended = false};
                            });
            stage_ = Stage::Commit;
            return Statement{.sql = kCommit, .params = {}};
        case Stage::CloseChat:
            stage_ = Stage::Commit;
            return Statement{.sql = kCommit, .params = {}};
        case Stage::Commit:
            done_(std::move(answer_));
            return std::nullopt;
        }
        return std::nullopt;
    }

    void abandon(DbError error) noexcept override { done_(std::unexpected(to_live_error(error))); }

private:
    enum class Stage : std::uint8_t { Begin, End, Read, CloseChat, Commit };

    core::LiveStreamId id_;
    LiveEnd reason_;
    core::WallTime at_;
    LiveCallback<EndedLiveStream> done_;
    LiveResult<EndedLiveStream> answer_ = std::unexpected(LiveStoreError::Unavailable);
    Stage stage_ = Stage::Begin;
};

} // namespace

class PgLiveStreams::Impl {
public:
    Impl(net::IReactor& reactor, std::unique_ptr<Pool> pool)
        : pool_(std::move(pool)), deferred_(reactor) {}

    Pool& pool() noexcept { return *pool_; }

    template <class T> void refuse(LiveCallback<T> done, LiveStoreError error) {
        deferred_.post(
            [done = std::move(done), error]() mutable noexcept { done(std::unexpected(error)); });
    }

private:
    std::unique_ptr<Pool> pool_;
    DeferredCalls deferred_;
};

std::expected<std::unique_ptr<PgLiveStreams>, std::string>
PgLiveStreams::create(net::IReactor& reactor, net::OffloadPool& offload,
                      const LiveStreamsConfig& config) {
    auto pool =
        Pool::create(reactor, offload,
                     PoolConfig{.conninfo = config.conninfo,
                                .application_name = "ulw-live",
                                .connections = std::clamp<std::size_t>(config.connections, 1, 8),
                                .connect_timeout = config.connect_timeout,
                                .request_timeout = config.request_timeout});
    if (!pool) {
        return std::unexpected(std::move(pool.error()));
    }
    return std::make_unique<PgLiveStreams>(Token{},
                                           std::make_unique<Impl>(reactor, std::move(*pool)));
}

PgLiveStreams::PgLiveStreams(Token /*token*/, std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

PgLiveStreams::~PgLiveStreams() = default;

void PgLiveStreams::create(core::ports::NewLiveStream stream, core::ports::LiveLimits limits,
                           LiveCallback<CreatedLiveStream> done) {
    impl_->pool().submit(
        std::make_unique<CreateStream>(std::move(stream), limits, std::move(done)));
}

void PgLiveStreams::find(const core::LiveStreamId& id, LiveCallback<LiveStream> done) {
    impl_->pool().submit(
        std::make_unique<Query>(Statement{.sql = kFind, .params = Params{}.add_uuid(id.uuid())},
                                [done = std::move(done)](Outcome outcome) mutable noexcept {
                                    if (!outcome) {
                                        done(std::unexpected(to_live_error(outcome.error())));
                                        return;
                                    }
                                    if (outcome->rows() == 0) {
                                        done(std::unexpected(LiveStoreError::NotFound));
                                        return;
                                    }
                                    done(decode_row(*outcome, 0));
                                }));
}

void PgLiveStreams::mark_live(const core::LiveStreamId& id, core::WallTime at,
                              LiveCallback<LiveStream> done) {
    impl_->pool().submit(std::make_unique<WriteThenRead<LiveStream>>(
        Statement{.sql = kMarkLive,
                  .params = Params{}.add_uuid(id.uuid()).add_int(micros_since_epoch(at))},
        Statement{.sql = kFind, .params = Params{}.add_uuid(id.uuid())},
        [](LiveResult<LiveStream> row, bool /*wrote*/) { return row; }, std::move(done)));
}

void PgLiveStreams::end(const core::LiveStreamId& id, LiveEnd reason, core::WallTime at,
                        LiveCallback<EndedLiveStream> done) {
    impl_->pool().submit(std::make_unique<EndStream>(id, reason, at, std::move(done)));
}

void PgLiveStreams::unfinished(std::size_t limit, LiveCallback<std::vector<LiveStream>> done) {
    if (limit == 0) {
        impl_->refuse(std::move(done), LiveStoreError::NotFound);
        return;
    }
    const auto bounded = static_cast<std::int64_t>(
        std::min<std::size_t>(limit, std::numeric_limits<std::int32_t>::max()));
    impl_->pool().submit(
        std::make_unique<Query>(Statement{.sql = kUnfinished, .params = Params{}.add_int(bounded)},
                                [done = std::move(done)](Outcome outcome) mutable noexcept {
                                    if (!outcome) {
                                        done(std::unexpected(to_live_error(outcome.error())));
                                        return;
                                    }
                                    done(decode_rows(*outcome));
                                }));
}

} // namespace infra::postgres
