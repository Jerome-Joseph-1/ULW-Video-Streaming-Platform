#pragma once

#include "core/models/ids.hpp"
#include "core/ports/notify.hpp"
#include "core/util/time.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Videos that became ready or failed, for the notification hook (ADR-0120): the catalog records
// each such change, and a chat node with the hook on takes them and sends them as events.
namespace core::ports {

struct VideoEvent {
    VideoId video;
    UserId owner;
    // Ready, or else failed.
    bool ready = false;
    std::int32_t version = 0;
    // A ready video's.
    std::optional<Millis> duration;
    // A failed video's.
    std::optional<std::string> error_reason;
    // When it changed, Unix milliseconds by the database's clock.
    std::int64_t at_ms = 0;
};

// The most events one take() answers: one hook request's worth.
inline constexpr std::size_t kMaxVideoEvents = 64;

class IVideoEvents {
public:
    virtual ~IVideoEvents() = default;
    // Up to `max` (at most kMaxVideoEvents) of the changes nobody took yet, oldest first. They
    // are taken for good: a second call, from this node or another, never answers them again.
    virtual void take_video_events(std::size_t max,
                                   NotifyCallback<std::vector<VideoEvent>> done) = 0;
};

} // namespace core::ports
