#include "infra/postgres/live_recordings.hpp"

#include "params.hpp"
#include "result.hpp"
#include "sql.hpp"
#include "sync_connection.hpp"

#include <chrono>
#include <utility>

namespace infra::postgres {

namespace {

// A packager's statements wait at most this long; it is restarted into them after that.
constexpr SessionSettings kRecordingSession{.application_name = "ulw-live-packager",
                                            .statement_timeout = core::Millis{10'000}};

constexpr Sql kFind = "SELECT video_id, failure FROM live_recordings WHERE stream_id = $1";

// The stream's row goes first and everything else hangs off it, so a second call for the stream
// inserts nothing at all. The job is what the gateway's upload commit queues.
constexpr Sql kRecord = R"sql(
WITH claimed AS (
    INSERT INTO live_recordings (stream_id, video_id) VALUES ($1, $2)
    ON CONFLICT (stream_id) DO NOTHING
    RETURNING video_id),
video AS (
    INSERT INTO videos (id, owner_id, title, state)
    SELECT video_id, $3, $4, 'processing' FROM claimed
    RETURNING id)
INSERT INTO jobs (video_id, kind, source_key, request_id)
SELECT id, 'transcode', $5, $6 FROM video)sql";

constexpr Sql kFail = R"sql(
INSERT INTO live_recordings (stream_id, failure) VALUES ($1, $2)
ON CONFLICT (stream_id) DO NOTHING)sql";

constexpr Sql kNotifyWorkers = "NOTIFY job_available";

std::unexpected<RecordingStoreError> unavailable() {
    return std::unexpected(RecordingStoreError::Unavailable);
}

std::expected<std::optional<RecordingRow>, RecordingStoreError> find_in(SyncConnection& conn,
                                                                        std::string_view stream) {
    const auto found = conn.exec(kFind, Params{}.add_text(stream));
    if (!found) {
        return unavailable();
    }
    if (found->rows() == 0) {
        return std::nullopt;
    }
    RecordingRow row;
    if (found->get(0, 0)) {
        row.video = domain_at<core::VideoId>(*found, 0, 0);
        if (!row.video) {
            return std::unexpected(RecordingStoreError::Corrupt);
        }
        return row;
    }
    row.failure = std::string(found->get(0, 1).value_or(""));
    if (row.failure.empty()) {
        return std::unexpected(RecordingStoreError::Corrupt);
    }
    return row;
}

// The row a write just made, or the one that was there first.
std::expected<RecordingRow, RecordingStoreError> row_after(SyncConnection& conn,
                                                           std::string_view stream) {
    auto row = find_in(conn, stream);
    if (!row) {
        return std::unexpected(row.error());
    }
    std::optional<RecordingRow>& found = *row;
    if (!found) {
        return std::unexpected(RecordingStoreError::Corrupt);
    }
    return std::move(*found);
}

} // namespace

std::expected<std::optional<RecordingRow>, RecordingStoreError>
PgLiveRecordings::find(std::string_view stream) {
    auto conn = SyncConnection::open(conninfo_, kRecordingSession);
    if (!conn) {
        return unavailable();
    }
    return find_in(*conn, stream);
}

std::expected<RecordingRow, RecordingStoreError>
PgLiveRecordings::record(const NewRecording& recording) {
    auto conn = SyncConnection::open(conninfo_, kRecordingSession);
    if (!conn) {
        return unavailable();
    }
    {
        auto tx = Transaction::begin(*conn);
        if (!tx) {
            return unavailable();
        }
        // The stream id doubles as the request id: it is what an operator traces the job by.
        const auto inserted = conn->exec(kRecord, Params{}
                                                      .add_text(recording.stream)
                                                      .add_uuid(recording.video.uuid())
                                                      .add_text(recording.owner.view())
                                                      .add_text(recording.title)
                                                      .add_text(recording.source.str())
                                                      .add_text(recording.stream));
        if (!inserted || !conn->exec(kNotifyWorkers) || !tx->commit()) {
            return unavailable();
        }
    }
    return row_after(*conn, recording.stream);
}

std::expected<RecordingRow, RecordingStoreError> PgLiveRecordings::fail(std::string_view stream,
                                                                        std::string_view reason) {
    auto conn = SyncConnection::open(conninfo_, kRecordingSession);
    if (!conn) {
        return unavailable();
    }
    if (!conn->exec(kFail, Params{}.add_text(stream).add_text(reason))) {
        return unavailable();
    }
    return row_after(*conn, stream);
}

} // namespace infra::postgres
