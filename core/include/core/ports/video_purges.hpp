#pragma once

#include "core/models/ids.hpp"
#include "core/ports/catalog.hpp"

#include <cstddef>
#include <expected>
#include <vector>

namespace core::ports {

// The catalog's side of purging deleted videos (ADR-0100): a video its owner deleted, or the
// operator's backend took down, is gone from every read at once; its stored objects and then its
// row go here, at the reaper's next pass. Blocking: for the reaper's own thread, never a
// reactor's.
class IVideoPurges {
public:
    virtual ~IVideoPurges() = default;
    // Up to `limit` deleted videos whose objects may be removed now, longest deleted first: none
    // has a transcode job queued or running. A job still queued for one is cancelled here, so it
    // is due at a later call; one running is left to finish, or to lapse and be cancelled then,
    // so that no worker writes a rendition after the objects are gone.
    [[nodiscard]] virtual std::expected<std::vector<VideoId>, CatalogError>
    due(std::size_t limit) = 0;
    // Forgets a deleted video for good, once its objects are gone: its row, and with it its
    // upload, jobs, renditions and grants, and its place in the queue. False, and nothing done,
    // when a job is queued or running for it again (a lapsed one put back), so that it comes due
    // again later and its objects are removed once more. A video no longer there leaves the queue.
    [[nodiscard]] virtual std::expected<bool, CatalogError> forget(const VideoId& video) = 0;
};

} // namespace core::ports
