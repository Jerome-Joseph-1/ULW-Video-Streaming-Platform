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
};

struct NewRecording {
    std::string stream;
    core::VideoId video;
    core::UserId owner;
    std::string title;
    core::StorageKey source;
};

// Turns an ended live stream into an ordinary video and its transcode job (ADR-0054), for
// the live packager, which has no reactor. Every call blocks and opens a session of its own:
// a packager makes two of them in its life.
class PgLiveRecordings {
public:
    explicit PgLiveRecordings(std::string conninfo) : conninfo_(std::move(conninfo)) {}

    // The video the stream already became, if it did.
    [[nodiscard]] std::expected<std::optional<core::VideoId>, RecordingStoreError>
    find(std::string_view stream);

    // One transaction: the stream's row, its video in processing, the video's transcode job, and
    // the wakeup for the workers. Returns the stream's video, which is `recording.video` unless
    // an earlier call recorded the stream first, in which case nothing was written.
    [[nodiscard]] std::expected<core::VideoId, RecordingStoreError>
    record(const NewRecording& recording);

private:
    std::string conninfo_;
};

} // namespace infra::postgres
