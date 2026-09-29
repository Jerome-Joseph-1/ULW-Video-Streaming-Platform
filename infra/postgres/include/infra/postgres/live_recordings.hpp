#pragma once

#include "core/models/ids.hpp"
#include "core/models/storage_key.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace infra::postgres {

enum class RecordingStoreError : std::uint8_t {
    // The database is unreachable, timed out or refused for now; the call may be repeated.
    Unavailable,
    // A stored row is not one this code writes.
    Corrupt,
    // The commit was sent and its answer lost, or the row could not be read after it: the write
    // may have landed, or may still land.
    Unknown,
};

// What a stream became: exactly one of a video, or the reason it could not be recorded.
struct RecordingRow {
    std::optional<core::VideoId> video;
    std::string failure;

    friend bool operator==(const RecordingRow&, const RecordingRow&) = default;
};

struct NewRecording {
    std::string stream;
    core::VideoId video;
    core::UserId owner;
    std::string title;
    core::StorageKey source;
};

// Turns an ended live stream into an ordinary video and its transcode job (ADR-0055), for
// the live packager, which has no reactor. Every call blocks and opens a session of its own:
// a packager makes a few in its life.
class PgLiveRecordings {
public:
    explicit PgLiveRecordings(std::string conninfo) : conninfo_(std::move(conninfo)) {}

    // What the stream already became, if anything.
    [[nodiscard]] std::expected<std::optional<RecordingRow>, RecordingStoreError>
    find(std::string_view stream);

    // One transaction: the stream's row, its video in processing, the video's transcode job, and
    // the wakeup for the workers. Returns the stream's row, which names `recording.video` unless
    // an earlier call recorded the stream first, in which case nothing was written.
    [[nodiscard]] std::expected<RecordingRow, RecordingStoreError>
    record(const NewRecording& recording);

    // The stream's row, saying it cannot be recorded and why; as record(), the first row wins.
    [[nodiscard]] std::expected<RecordingRow, RecordingStoreError> fail(std::string_view stream,
                                                                        std::string_view reason);

private:
    std::string conninfo_;
};

} // namespace infra::postgres
