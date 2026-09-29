#pragma once

#include "core/models/ids.hpp"
#include "core/ports/clock.hpp"
#include "core/ports/object_stream.hpp"
#include "core/ports/object_transfer.hpp"
#include "core/ports/random.hpp"
#include "infra/ffmpeg/recording_remux.hpp"
#include "infra/postgres/live_recordings.hpp"

#include "stream_id.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string_view>

namespace live {

// Where a stream's recording is kept: beside its segments, under the prefix the bucket's
// lifecycle rule expires.
inline constexpr std::string_view kRecordingName = "recording.ts";

enum class RecordError : std::uint8_t {
    // The stored playlist does not parse, or names an init segment that is not ours.
    PlaylistInvalid,
    // The store could not be read: the playlist, a segment, or whether a segment exists.
    StoreUnreadable,
    // ffmpeg could not be started or refused what it was given.
    RemuxFailed,
    // The store did not take the recording.
    UploadFailed,
    // The process was asked to stop first.
    Stopped,
    // The database could not be reached or answered with something not ours.
    DatabaseUnavailable,
};

[[nodiscard]] std::string_view to_string(RecordError e) noexcept;

struct RecorderDeps {
    core::ports::IObjectTransfer& store;
    core::ports::IObjectStreams& streams;
    const infra::ffmpeg::RecordingRemuxer& remuxer;
    infra::postgres::PgLiveRecordings& recordings;
    const core::ports::IClock& clock;
    core::ports::IRandom& random;
};

struct RecorderSettings {
    StreamId stream;
    core::UserId owner;
    // Holds one segment at a time on its way to ffmpeg, and is the children's writable dir.
    std::filesystem::path work_dir;
    // The longest the remux may run: that of the stream it copies.
    core::Seconds budget{};
};

// Once the stream's stored playlist ends: the recording, and the one video and job it becomes.
// Every step can be repeated after a crash: a stream already recorded is left alone, a
// recording already in the store is not made again, and the video and job are written together
// with the stream's row, once. nullopt: the stream has not ended, or ended without a segment,
// and there is nothing to record.
[[nodiscard]] std::expected<std::optional<core::VideoId>, RecordError>
record_stream(const RecorderDeps& deps, const RecorderSettings& settings,
              const std::stop_token& stop);

} // namespace live
