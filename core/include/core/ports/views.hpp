#pragma once

#include "core/models/ids.hpp"
#include "core/ports/catalog.hpp"
#include "core/util/time.hpp"

#include <vector>

namespace core::ports {

// A viewer started playing a video: they fetched its master playlist.
struct ViewEvent {
    VideoId video;
    UserId viewer;
    WallTime at;
};

// Where view events are kept. They feed analytics, not playback, so a batch that fails is
// reported and dropped rather than retried.
class IViewLog {
public:
    virtual ~IViewLog() = default;
    // One statement for the whole batch; `done` is called as a catalog callback is.
    virtual void record_views(std::vector<ViewEvent> batch, CatalogCallback<void> done) = 0;
};

} // namespace core::ports
