#include "reaper.hpp"

#include <format>
#include <optional>

namespace reaper {

namespace {

// True when the store holds nothing for the upload any more. The catalog row is aborted before
// this runs, so a commit that finished in the store just ahead of the abort has left a whole
// object at the upload's key that no video will ever reference: it goes too, after the session,
// so that a commit still in flight can no longer complete one.
std::optional<std::string> release(core::ports::IIngestStore& store,
                                   core::ports::IObjectAdmin& admin,
                                   const core::ports::IngestId& ingest) {
    using core::ports::StorageError;
    store.discard(ingest);
    if (const auto removed = admin.remove(ingest.key);
        !removed && removed.error() != StorageError::NotFound) {
        return std::format("remove {}: {}", ingest.key.view(),
                           core::ports::to_string(removed.error()));
    }
    // Neither an open session nor a finished object answers a durable offset.
    const auto left = store.durable_offset(ingest);
    if (left) {
        return std::format("release {}: the store still holds it", ingest.key.view());
    }
    if (left.error() != StorageError::NotFound) {
        return std::format("release {}: {}", ingest.key.view(),
                           core::ports::to_string(left.error()));
    }
    return std::nullopt;
}

void expire_uploads(core::ports::IUploadExpiry& uploads, core::ports::IIngestStore& store,
                    core::ports::IObjectAdmin& admin, const core::ports::IClock& clock,
                    const Options& options, Report& report) {
    while (true) {
        auto expired = uploads.expire(clock.wall_now(), options.batch);
        if (!expired) {
            report.problems.push_back(
                std::format("expire uploads: {}", core::ports::to_string(expired.error())));
            return;
        }
        for (const core::ports::ExpiredUpload& upload : *expired) {
            if (const auto problem = release(store, admin, upload.ingest)) {
                report.problems.push_back(*problem);
                ++report.uploads_release_failed;
            } else {
                ++report.uploads_expired;
            }
        }
        // A short batch means the rest were busy or there are no more.
        if (expired->size() < options.batch) {
            return;
        }
    }
}

void forget_rooms(core::ports::IUnusedRooms& rooms, const core::ports::IClock& clock,
                  const Options& options, Report& report) {
    const core::WallTime before = clock.wall_now() - options.unused_room_after;
    // A batch of rooms looked at per statement, from where the last pass stopped, until the walk
    // reaches the cutoff or the pass has looked at its share.
    for (std::size_t walked = 0; walked < options.rooms_per_pass; walked += options.batch) {
        const auto scan = rooms.forget_unused(before, options.batch);
        if (!scan) {
            report.problems.push_back(
                std::format("forget unused chat rooms: {}", core::ports::to_string(scan.error())));
            return;
        }
        report.rooms_forgotten += scan->forgotten;
        if (scan->finished) {
            return;
        }
    }
}

} // namespace

Report run_once(core::ports::IUploadExpiry& uploads, core::ports::IIngestStore& store,
                core::ports::IObjectAdmin& admin, core::ports::IUnusedRooms& rooms,
                const core::ports::IClock& clock, const Options& options) {
    Report report;
    expire_uploads(uploads, store, admin, clock, options, report);
    const auto swept = admin.reap_abandoned(clock.wall_now() - options.orphan_after);
    if (swept) {
        report.parts_orphaned = *swept;
    } else {
        report.problems.push_back(
            std::format("sweep orphaned uploads: {}", core::ports::to_string(swept.error())));
    }
    forget_rooms(rooms, clock, options, report);
    return report;
}

std::string metrics_text(const Report& report) {
    return std::format("# TYPE reaper_uploads_expired_last_run gauge\n"
                       "reaper_uploads_expired_last_run {}\n"
                       "# TYPE reaper_uploads_release_failed_last_run gauge\n"
                       "reaper_uploads_release_failed_last_run {}\n"
                       "# TYPE reaper_parts_orphaned_last_run gauge\n"
                       "reaper_parts_orphaned_last_run {}\n"
                       "# TYPE reaper_chat_rooms_forgotten_last_run gauge\n"
                       "reaper_chat_rooms_forgotten_last_run {}\n",
                       report.uploads_expired, report.uploads_release_failed, report.parts_orphaned,
                       report.rooms_forgotten);
}

} // namespace reaper
