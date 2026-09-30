#pragma once

#include "core/ports/object_transfer.hpp"

#include "media_playlist.hpp"
#include "stream_id.hpp"

#include <cstdint>
#include <expected>
#include <vector>

namespace live {

// Segments first..last of one epoch, which the recording takes in order after its init segment.
struct RecordingRun {
    std::uint32_t epoch = 0;
    std::uint64_t first = 0;
    std::uint64_t last = 0;

    friend bool operator==(const RecordingRun&, const RecordingRun&) = default;
};

struct RecordingPlan {
    // In stream order; one per stretch of the stream a run published.
    std::vector<RecordingRun> runs;
    // Sequence numbers no epoch has a segment for. Every one the packager published is in the
    // store, so these are holes left by something else, and the recording goes on without them.
    std::uint64_t missing = 0;
};

enum class PlanError : std::uint8_t {
    // The playlist does not end: the stream is live, or was drained and not ended.
    NotEnded,
    // It ends without a segment; there is nothing to record.
    Empty,
    // A segment's init segment does not name an epoch.
    PlaylistInvalid,
    // The store could not say whether a segment exists.
    StoreUnreadable,
};

// Which run's segment each place in the stream holds, from the ended playlist and what the
// store has. The playlist names the owners of the last window only. Older places are walked
// from the newest down, each taken by the newest epoch holding a segment for it: runs are
// numbered in the order they claimed the stream and each continued where the stored playlist
// ended, so a newer epoch's segment for a place is the published one, and an older epoch's is
// what a superseded run wrote before it stopped. One size() per place, plus one per epoch
// stepped over.
[[nodiscard]] std::expected<RecordingPlan, PlanError>
plan_recording(const MediaPlaylist& ended, const StreamId& stream,
               core::ports::IObjectTransfer& store);

} // namespace live
