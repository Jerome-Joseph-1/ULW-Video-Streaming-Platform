#include "reaper.hpp"

#include <format>
#include <optional>
#include <string>

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

// Removes every object a deleted video left: the source and the HLS renditions, all under
// videos/<id>/. A key gone already is fine: a pass that stopped halfway runs again.
std::optional<std::string> remove_objects(core::ports::IObjectAdmin& admin,
                                          const core::VideoId& video) {
    using core::ports::StorageError;
    const std::string prefix = "videos/" + video.to_string() + "/";
    const auto keys = admin.list(prefix);
    if (!keys) {
        return std::format("list {}: {}", prefix, core::ports::to_string(keys.error()));
    }
    for (const core::StorageKey& key : *keys) {
        if (const auto removed = admin.remove(key);
            !removed && removed.error() != StorageError::NotFound) {
            return std::format("remove {}: {}", key.view(),
                               core::ports::to_string(removed.error()));
        }
    }
    return std::nullopt;
}

void purge_videos(core::ports::IVideoPurges& purges, core::ports::IObjectAdmin& admin,
                  const Options& options, Report& report) {
    for (std::size_t looked = 0; looked < options.videos_per_pass; looked += options.batch) {
        const auto due = purges.due(options.batch);
        if (!due) {
            report.problems.push_back(
                std::format("purge deleted videos: {}", core::ports::to_string(due.error())));
            return;
        }
        bool all_purged = true;
        for (const core::VideoId& video : *due) {
            if (const auto problem = remove_objects(admin, video)) {
                report.problems.push_back(*problem);
                ++report.videos_purge_failed;
                all_purged = false;
                continue;
            }
            const auto forgotten = purges.forget(video);
            if (!forgotten) {
                report.problems.push_back(std::format("forget deleted video {}: {}",
                                                      video.to_string(),
                                                      core::ports::to_string(forgotten.error())));
                ++report.videos_purge_failed;
                all_purged = false;
                continue;
            }
            if (!*forgotten) {
                // A job was put back for it: it comes due again once that is cancelled.
                all_purged = false;
                continue;
            }
            ++report.videos_purged;
        }
        // A short batch means there are no more due; one that left some behind would only find
        // them again.
        if (due->size() < options.batch || !all_purged) {
            return;
        }
    }
}

} // namespace

Report run_once(core::ports::IUploadExpiry& uploads, core::ports::IIngestStore& store,
                core::ports::IObjectAdmin& admin, core::ports::IUnusedRooms& rooms,
                core::ports::IVideoPurges& purges, const core::ports::IClock& clock,
                const Options& options) {
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
    purge_videos(purges, admin, options, report);
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
                       "reaper_chat_rooms_forgotten_last_run {}\n"
                       "# TYPE reaper_videos_purged_last_run gauge\n"
                       "reaper_videos_purged_last_run {}\n"
                       "# TYPE reaper_videos_purge_failed_last_run gauge\n"
                       "reaper_videos_purge_failed_last_run {}\n",
                       report.uploads_expired, report.uploads_release_failed, report.parts_orphaned,
                       report.rooms_forgotten, report.videos_purged, report.videos_purge_failed);
}

} // namespace reaper
