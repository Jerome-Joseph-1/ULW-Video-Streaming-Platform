#pragma once

#include "core/ports/clock.hpp"
#include "core/ports/storage.hpp"
#include "core/ports/unused_rooms.hpp"
#include "core/ports/upload_expiry.hpp"
#include "core/ports/video_purges.hpp"
#include "core/util/time.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace reaper {

struct Options {
    // How many uploads one catalog call expires; a full batch is followed by another.
    std::size_t batch = 100;
    // How old a storage session with no catalog row must be before it is aborted: longer than
    // any upload may live, so that no active upload's session can be mistaken for an orphan.
    core::Seconds orphan_after{};
    // How long a chat room recorded by a join, and never used, is kept: long enough that a
    // room whose members are being added just after its first join is used by then.
    core::Seconds unused_room_after{std::chrono::hours(24)};
    // Chat rooms looked at per pass, `batch` per statement, from where the last pass stopped: a
    // pass costs the same however many rooms there are, and a lap over a million takes a day of
    // passes each 15 minutes (ADR-0075).
    std::size_t rooms_per_pass = 10'000;
    // Deleted videos purged per pass at most, `batch` per catalog call (ADR-0100): the rest wait
    // for the next pass.
    std::size_t videos_per_pass = 1'000;
};

struct Report {
    // Aborted in the catalog and gone from the store: session released, object removed.
    std::size_t uploads_expired = 0;
    // Aborted in the catalog, but the store did not confirm the release. They are not retried
    // as uploads; the sweep gets the session once it is old enough.
    std::size_t uploads_release_failed = 0;
    std::size_t parts_orphaned = 0;
    // Chat rooms a refused join recorded and nothing used, forgotten.
    std::size_t rooms_forgotten = 0;
    // Deleted videos whose stored objects were removed and whose rows are gone.
    std::size_t videos_purged = 0;
    // Deleted videos whose objects could not all be removed, or whose row stayed: tried again at
    // the next pass.
    std::size_t videos_purge_failed = 0;
    // What went wrong, one line each. The phases are independent, so one failing does not stop
    // the other.
    std::vector<std::string> problems;
};

// One pass: uploads past their expires_at are aborted in the catalog and their storage sessions
// released, then sessions no upload owns are swept, then chat rooms nothing used are forgotten
// (up to Options::rooms_per_pass looked at), then deleted videos are purged: every object under
// videos/<id>/ removed, then the row (up to Options::videos_per_pass).
[[nodiscard]] Report run_once(core::ports::IUploadExpiry& uploads, core::ports::IIngestStore& store,
                              core::ports::IObjectAdmin& admin, core::ports::IUnusedRooms& rooms,
                              core::ports::IVideoPurges& purges, const core::ports::IClock& clock,
                              const Options& options);

// Prometheus text format, for whatever collects the pass's output.
[[nodiscard]] std::string metrics_text(const Report& report);

} // namespace reaper
