#pragma once

#include "core/models/ids.hpp"
#include "core/ports/clock.hpp"
#include "core/ports/object_stream.hpp"
#include "core/ports/object_transfer.hpp"
#include "core/ports/random.hpp"

#include "recording_ports.hpp"
#include "stream_id.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace live {

enum class RecordOutcome : std::uint8_t {
    // This run recorded the stream: its video and job are queued.
    Recorded,
    // The stream already has its row, a video or a mark that it cannot be recorded; nothing
    // was written.
    AlreadyRecorded,
    // The stored playlist does not end, or ends without a segment.
    NothingToRecord,
    // A newer packager of the stream holds a claim above the ended playlist's last run, or the
    // playlist changed under the recording: the end this run saw was a stale writer's, and
    // the newer packager records the stream when it ends.
    Superseded,
    // The stream cannot be recorded, whatever is retried: its segments are gone or ffmpeg
    // refuses them. Once its end has been confirmed, the stream's row says so, and no run
    // tries again.
    Unrecordable,
    // Something that may pass failed (the store, the database, the sandbox), or the process was
    // asked to stop: nothing was queued, and the next run for the stream tries again.
    Failed,
};

[[nodiscard]] std::string_view to_string(RecordOutcome outcome) noexcept;

struct RecordResult {
    RecordOutcome outcome = RecordOutcome::Failed;
    // The stream's video, when it has one.
    std::optional<core::VideoId> video;
    // Why, for the log, when the outcome is not Recorded.
    std::string detail;
};

struct RecorderDeps {
    core::ports::IObjectTransfer& store;
    core::ports::IObjectStreams& streams;
    IRecordingCopier& copier;
    IRecordingCatalog& catalog;
    const core::ports::IClock& clock;
    core::ports::IRandom& random;
};

struct RecorderSettings {
    StreamId stream;
    core::UserId owner;
    // Holds the segment on its way to ffmpeg and the children's empty writable directory.
    std::filesystem::path work_dir;
    // The longest the copies may run: that of the stream they copy.
    core::Seconds budget{};
    // The most the recording can be (recording_bound); the store sizes its pieces from it.
    std::uint64_t max_bytes = 0;
    // The epoch this process claimed, when it found the stream ended right after its claim.
    std::optional<std::uint32_t> own_claim;
};

// The most the recording of a stream capped at `max_kbps` for `max_duration` can be: the media
// plus an eighth for the TS packets and the audio a stand-in adds.
[[nodiscard]] std::uint64_t recording_bound(std::uint32_t max_kbps,
                                            core::Seconds max_duration) noexcept;

// Once the stream's stored playlist ends: the recording, streamed into the store under the
// source key of a video id made for it, and the one video and job it becomes (ADR-0054). Every
// step can be repeated: a stream with a row is left alone, and a run that loses the race for
// the row, or finds the playlist changed, removes what it stored.
[[nodiscard]] RecordResult record_stream(const RecorderDeps& deps, const RecorderSettings& settings,
                                         const std::stop_token& stop);

} // namespace live
