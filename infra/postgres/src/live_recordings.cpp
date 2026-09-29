#include "infra/postgres/live_recordings.hpp"

#include "params.hpp"
#include "result.hpp"
#include "sql.hpp"
#include "sync_connection.hpp"

#include <chrono>

namespace infra::postgres {

namespace {

// A packager's two statements wait at most this long; it is restarted into them after that.
constexpr SessionSettings kRecordingSession{.application_name = "ulw-live-packager",
                                            .statement_timeout = core::Millis{10'000}};

constexpr Sql kFind = "SELECT video_id FROM live_recordings WHERE stream_id = $1";

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

constexpr Sql kNotifyWorkers = "NOTIFY job_available";

std::unexpected<RecordingStoreError> unavailable() {
    return std::unexpected(RecordingStoreError::Unavailable);
}

std::expected<std::optional<core::VideoId>, RecordingStoreError> find_in(SyncConnection& conn,
                                                                         std::string_view stream) {
    const auto found = conn.exec(kFind, Params{}.add_text(stream));
    if (!found) {
        return unavailable();
    }
    if (found->rows() == 0) {
        return std::nullopt;
    }
    auto video = domain_at<core::VideoId>(*found, 0, 0);
    if (!video) {
        return std::unexpected(RecordingStoreError::Corrupt);
    }
    return *video;
}

} // namespace

std::expected<std::optional<core::VideoId>, RecordingStoreError>
PgLiveRecordings::find(std::string_view stream) {
    auto conn = SyncConnection::open(conninfo_, kRecordingSession);
    if (!conn) {
        return unavailable();
    }
    return find_in(*conn, stream);
}

std::expected<core::VideoId, RecordingStoreError>
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
    auto video = find_in(*conn, recording.stream);
    if (!video) {
        return std::unexpected(video.error());
    }
    if (!*video) {
        return std::unexpected(RecordingStoreError::Corrupt);
    }
    return **video;
}

} // namespace infra::postgres
