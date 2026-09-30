#include "recording_plan.hpp"

#include "core/models/storage_key.hpp"
#include "infra/ffmpeg/live_remux.hpp"

#include <algorithm>
#include <optional>

namespace live {

namespace {

// Whether the store holds segment `sequence` of `epoch`.
std::expected<bool, PlanError> stored(core::ports::IObjectTransfer& store, const StreamId& stream,
                                      std::uint32_t epoch, std::uint64_t sequence) {
    const auto key = core::StorageKey::parse(stream.key_prefix() +
                                             infra::ffmpeg::live_segment_name(epoch, sequence));
    if (!key) {
        return std::unexpected(PlanError::PlaylistInvalid);
    }
    const auto size = store.size(*key);
    if (size) {
        return true;
    }
    if (size.error() == core::ports::StorageError::NotFound) {
        return false;
    }
    return std::unexpected(PlanError::StoreUnreadable);
}

// Appends `sequence` of `epoch` to runs kept newest first.
void take(std::vector<RecordingRun>& newest_first, std::uint32_t epoch, std::uint64_t sequence) {
    if (!newest_first.empty() && newest_first.back().epoch == epoch &&
        newest_first.back().first == sequence + 1) {
        newest_first.back().first = sequence;
        return;
    }
    newest_first.push_back({.epoch = epoch, .first = sequence, .last = sequence});
}

} // namespace

std::expected<RecordingPlan, PlanError> plan_recording(const MediaPlaylist& ended,
                                                       const StreamId& stream,
                                                       core::ports::IObjectTransfer& store) {
    if (!ended.ended) {
        return std::unexpected(PlanError::NotEnded);
    }
    if (ended.segments.empty()) {
        return std::unexpected(PlanError::Empty);
    }
    RecordingPlan plan;
    std::vector<RecordingRun> newest_first;
    std::uint32_t epoch = 0;
    for (std::size_t i = ended.segments.size(); i-- > 0;) {
        const auto owner = infra::ffmpeg::live_init_epoch(ended.segments[i].init);
        if (!owner) {
            return std::unexpected(PlanError::PlaylistInvalid);
        }
        epoch = *owner;
        take(newest_first, epoch, ended.media_sequence + i);
    }
    for (std::uint64_t sequence = ended.media_sequence; sequence-- > 0;) {
        std::optional<std::uint32_t> owner;
        for (std::int64_t candidate = epoch; candidate >= 0; --candidate) {
            const auto held =
                stored(store, stream, static_cast<std::uint32_t>(candidate), sequence);
            if (!held) {
                return std::unexpected(held.error());
            }
            if (*held) {
                owner = static_cast<std::uint32_t>(candidate);
                break;
            }
        }
        if (!owner) {
            ++plan.missing;
            continue;
        }
        epoch = *owner;
        take(newest_first, epoch, sequence);
    }
    plan.runs.assign(newest_first.rbegin(), newest_first.rend());
    return plan;
}

} // namespace live
