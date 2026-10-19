#pragma once

#include "core/ports/clock.hpp"
#include "core/ports/random.hpp"
#include "core/ports/video_events.hpp"
#include "net/reactor.hpp"

#include "notify_sender.hpp"

#include <cstdint>
#include <memory>
#include <string>

// Videos that became ready or failed, sent on the notification hook (ADR-0120): every chat node
// with the hook on takes the catalog's changes, a batch at a time, and offers each as an event.
// Reactor thread only.
namespace chat {

struct VideoRelayLimits {
    // How often the catalog is asked when the last take found less than a full batch, or failed.
    core::Millis poll_every{2'000};
};

struct VideoRelayCounters {
    // Events handed to the sender.
    std::uint64_t events = 0;
    std::uint64_t take_failures = 0;
    // Changes taken but never offered: their event could not be made.
    std::uint64_t allocation_failures = 0;
};

class VideoRelay final : public net::ITimerHandler {
public:
    VideoRelay(net::IReactor& reactor, core::ports::IVideoEvents& store, IEventSink& sink,
               const core::ports::IClock& clock, core::ports::IRandom& random,
               VideoRelayLimits limits = {});
    ~VideoRelay() override;
    VideoRelay(const VideoRelay&) = delete;
    VideoRelay& operator=(const VideoRelay&) = delete;
    VideoRelay(VideoRelay&&) = delete;
    VideoRelay& operator=(VideoRelay&&) = delete;

    // Takes the first batch now. After stop(), nothing more is taken, and an answer still to come
    // is offered nowhere: the node is going, and those changes are lost, as any event in flight.
    void start() noexcept;
    void stop() noexcept;

    void on_timeout() noexcept override;

    [[nodiscard]] const VideoRelayCounters& counters() const noexcept { return counters_; }

private:
    void take() noexcept;
    void taken(core::ports::NotifyResult<std::vector<core::ports::VideoEvent>> result) noexcept;
    [[nodiscard]] std::string event_text(const core::ports::VideoEvent& event);

    net::IReactor& reactor_;
    core::ports::IVideoEvents& store_;
    IEventSink& sink_;
    const core::ports::IClock& clock_;
    core::ports::IRandom& random_;
    VideoRelayLimits limits_;
    VideoRelayCounters counters_;
    // Store answers that come after the relay is gone find this expired.
    std::shared_ptr<VideoRelay*> self_;
    net::TimerId timer_;
    bool started_ = false;
    bool stopped_ = false;
};

} // namespace chat
