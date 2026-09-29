#pragma once

#include "core/models/ids.hpp"
#include "core/ports/catalog.hpp"
#include "core/ports/storage.hpp"
#include "core/util/time.hpp"

#include <cstddef>
#include <expected>
#include <vector>

namespace core::ports {

struct ExpiredUpload {
    UploadId id;
    // What the store needs to release the upload's session.
    IngestId ingest;
};

// The catalog's side of the upload reaper. Blocking: for the reaper's own thread, never a
// reactor's.
class IUploadExpiry {
public:
    virtual ~IUploadExpiry() = default;
    // Aborts up to `limit` uploads that are still active at their expires_at, fails their
    // videos, and returns them. Each abort is one statement, so a commit racing it either
    // completes the upload first, and then it is left alone, or finds it aborted. An upload
    // whose owner is streaming into it holds its claim and is skipped until the next call.
    [[nodiscard]] virtual std::expected<std::vector<ExpiredUpload>, CatalogError>
    expire(WallTime now, std::size_t limit) = 0;
};

} // namespace core::ports
