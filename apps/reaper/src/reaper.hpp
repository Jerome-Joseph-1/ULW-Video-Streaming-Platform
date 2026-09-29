#pragma once

#include "core/ports/clock.hpp"
#include "core/ports/storage.hpp"
#include "core/ports/upload_expiry.hpp"
#include "core/util/time.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace reaper {

struct Options {
    // How many uploads one catalog call expires; a full batch is followed by another.
    std::size_t batch = 100;
    // How old a multipart upload with no catalog row must be before it is aborted: longer than
    // any upload may live, so that no active upload's session can be mistaken for an orphan.
    core::Seconds orphan_after{};
};

struct Report {
    std::size_t uploads_expired = 0;
    std::size_t parts_orphaned = 0;
    // What went wrong, one line each. The phases are independent, so one failing does not stop
    // the other.
    std::vector<std::string> problems;
};

// One pass: uploads past their expires_at are aborted in the catalog and their storage sessions
// released, then sessions no upload owns are swept.
[[nodiscard]] Report run_once(core::ports::IUploadExpiry& uploads, core::ports::IIngestStore& store,
                              core::ports::IObjectAdmin& admin, const core::ports::IClock& clock,
                              const Options& options);

// Prometheus text format, for whatever collects the pass's output.
[[nodiscard]] std::string metrics_text(const Report& report);

} // namespace reaper
