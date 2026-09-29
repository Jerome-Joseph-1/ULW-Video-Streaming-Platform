#include "reaper.hpp"

#include <format>

namespace reaper {

namespace {

void expire_uploads(core::ports::IUploadExpiry& uploads, core::ports::IIngestStore& store,
                    const core::ports::IClock& clock, const Options& options, Report& report) {
    while (true) {
        auto expired = uploads.expire(clock.wall_now(), options.batch);
        if (!expired) {
            report.problems.push_back(
                std::format("expire uploads: {}", core::ports::to_string(expired.error())));
            return;
        }
        for (const core::ports::ExpiredUpload& upload : *expired) {
            // Already aborted in the catalog. A session this cannot release stays open in the
            // store until the sweep below, or the bucket's lifecycle rule, gets to it.
            store.discard(upload.ingest);
            ++report.uploads_expired;
        }
        // A short batch means the rest were busy or there are no more.
        if (expired->size() < options.batch) {
            return;
        }
    }
}

} // namespace

Report run_once(core::ports::IUploadExpiry& uploads, core::ports::IIngestStore& store,
                core::ports::IObjectAdmin& admin, const core::ports::IClock& clock,
                const Options& options) {
    Report report;
    expire_uploads(uploads, store, clock, options, report);
    const auto swept = admin.reap_abandoned(clock.wall_now() - options.orphan_after);
    if (swept) {
        report.parts_orphaned = *swept;
    } else {
        report.problems.push_back(
            std::format("sweep orphaned uploads: {}", core::ports::to_string(swept.error())));
    }
    return report;
}

std::string metrics_text(const Report& report) {
    return std::format("# TYPE uploads_expired_total counter\n"
                       "uploads_expired_total {}\n"
                       "# TYPE parts_orphaned_total counter\n"
                       "parts_orphaned_total {}\n",
                       report.uploads_expired, report.parts_orphaned);
}

} // namespace reaper
