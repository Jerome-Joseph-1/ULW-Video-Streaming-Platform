#include "infra/postgres/live_streams.hpp"

#include "deferred_calls.hpp"
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
// The row, unless the owner has an unfinished stream already or `max` are unfinished; and the
// stream's live chat opened in the same statement (ADR-0070: whatever starts a stream opens its
// chat), unless its room exists as something else, which a fresh stream id never names.
constexpr Sql kCreate = R"sql(
WITH s AS (
    INSERT INTO live_streams (id, owner_id, srt_passphrase, created_at)
    SELECT $1, $2, $3, timestamptz 'epoch' + $4 * interval '1 microsecond'
     WHERE (SELECT count(*) FROM live_streams WHERE state <> 'ended') < $5
    ON CONFLICT (owner_id) WHERE state <> 'ended' DO NOTHING
    RETURNING *),
chat AS (
    INSERT INTO chat_rooms (room_id, kind)
    SELECT live_chat_room(s.id::text), 'stream_live_chat' FROM s
     WHERE NOT EXISTS (SELECT 1 FROM chat_members WHERE room_id = live_chat_room(s.id::text))
       AND NOT EXISTS (SELECT 1 FROM room_state
                        WHERE room_id = live_chat_room(s.id::text)
                          AND kind <> 'stream_live_chat')
    ON CONFLICT (room_id) DO NOTHING)
SELECT s.id, s.owner_id, s.state, (extract(epoch FROM s.created_at) * 1000000)::bigint,
       (extract(epoch FROM s.live_at) * 1000000)::bigint,
       (extract(epoch FROM s.ended_at) * 1000000)::bigint, s.end_reason, s.srt_passphrase,
       (SELECT r.video_id FROM live_recordings r WHERE r.stream_id = s.id::text)
  FROM s)sql";

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
    for (const LiveEnd e : {LiveEnd::Owner, LiveEnd::Finished, LiveEnd::Failed, LiveEnd::Timeout}) {
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

void PgLiveStreams::create(core::ports::NewLiveStream stream, std::uint32_t max_unfinished,
                           LiveCallback<CreatedLiveStream> done) {
    // The parameters borrow their text, and the operation keeps its statements for reruns: the
    // owner and passphrase live in the decision below, which lives as long as the operation.
    auto owner = std::make_unique<core::UserId>(stream.owner);
    auto passphrase = std::make_unique<std::string>(std::move(stream.passphrase));
    Statement write{.sql = kCreate,
                    .params = Params{}
                                  .add_uuid(stream.id.uuid())
                                  .add_text(owner->view())
                                  .add_text(*passphrase)
                                  .add_int(micros_since_epoch(stream.at))
                                  .add_int(static_cast<std::int64_t>(max_unfinished))};
    Statement read{.sql = kCurrent, .params = Params{}.add_text(owner->view())};
    impl_->pool().submit(std::make_unique<WriteThenRead<CreatedLiveStream>>(
        write, read,
        [owner = std::move(owner), passphrase = std::move(passphrase)](
            LiveResult<LiveStream> row, bool wrote) -> LiveResult<CreatedLiveStream> {
            if (!row) {
                // No row of the owner's either: as many streams are unfinished as are allowed.
                return std::unexpected(
                    row.error() == LiveStoreError::NotFound ? LiveStoreError::Full : row.error());
            }
            return CreatedLiveStream{.stream = std::move(*row), .created = wrote};
        },
        std::move(done)));
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
    impl_->pool().submit(std::make_unique<WriteThenRead<EndedLiveStream>>(
        Statement{.sql = kEnd,
                  .params = Params{}
                                .add_uuid(id.uuid())
                                .add_text(core::ports::to_string(reason))
                                .add_int(micros_since_epoch(at))},
        Statement{.sql = kFind, .params = Params{}.add_uuid(id.uuid())},
        [](LiveResult<LiveStream> row, bool wrote) -> LiveResult<EndedLiveStream> {
            if (!row) {
                return std::unexpected(row.error());
            }
            return EndedLiveStream{.stream = std::move(*row), .ended = wrote};
        },
        std::move(done)));
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
