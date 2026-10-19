#include "video_relay.hpp"

#include "core/util/json.hpp"
#include "core/util/uuid.hpp"

#include <exception>
#include <format>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace chat {

using core::ports::NotifyResult;
using core::ports::VideoEvent;

VideoRelay::VideoRelay(net::IReactor& reactor, core::ports::IVideoEvents& store, IEventSink& sink,
                       const core::ports::IClock& clock, core::ports::IRandom& random,
                       VideoRelayLimits limits)
    : reactor_(reactor), store_(store), sink_(sink), clock_(clock), random_(random),
      limits_(limits), self_(std::make_shared<VideoRelay*>(this)) {}

VideoRelay::~VideoRelay() {
    reactor_.cancel_timer(timer_);
}

void VideoRelay::start() noexcept {
    if (started_ || stopped_) {
        return;
    }
    started_ = true;
    take();
}

void VideoRelay::stop() noexcept {
    stopped_ = true;
    reactor_.cancel_timer(timer_);
    timer_ = {};
}

void VideoRelay::on_timeout() noexcept {
    timer_ = {};
    take();
}

void VideoRelay::take() noexcept {
    if (stopped_) {
        return;
    }
    store_.take_video_events(core::ports::kMaxVideoEvents,
                             [weak = std::weak_ptr(self_)](
                                 NotifyResult<std::vector<VideoEvent>> result) mutable noexcept {
                                 if (const auto self = weak.lock()) {
                                     (*self)->taken(std::move(result));
                                 }
                             });
}

void VideoRelay::taken(NotifyResult<std::vector<VideoEvent>> result) noexcept {
    if (stopped_) {
        return;
    }
    if (!result) {
        ++counters_.take_failures;
        timer_ = reactor_.arm_timer(limits_.poll_every, *this);
        return;
    }
    for (const VideoEvent& event : *result) {
        try {
            sink_.offer(event_text(event));
            ++counters_.events;
        } catch (const std::exception&) {
            // Lost, as an event the sender's full queue drops, and counted.
            ++counters_.allocation_failures;
        }
    }
    // A full batch says more wait: the next is taken at once.
    if (result->size() == core::ports::kMaxVideoEvents) {
        take();
        return;
    }
    timer_ = reactor_.arm_timer(limits_.poll_every, *this);
}

std::string VideoRelay::event_text(const VideoEvent& event) {
    std::string out = R"({"id":")";
    out += core::Uuid::v7(clock_, random_).to_string();
    out += R"(","type":")";
    out += event.ready ? "video_ready" : "video_failed";
    // When the video changed, not when it was taken: a node down for a while sends late.
    std::format_to(std::back_inserter(out), R"(","at":{},"video":")", event.at_ms / 1000);
    out += event.video.to_string();
    out += R"(","owner":)";
    core::json::append_string(out, event.owner.view());
    std::format_to(std::back_inserter(out), R"(,"version":{})", event.version);
    if (event.ready) {
        std::format_to(std::back_inserter(out), R"(,"duration_ms":{})",
                       event.duration ? event.duration->count() : 0);
    } else {
        out += R"(,"error_reason":)";
        core::json::append_string(out, event.error_reason.value_or(""));
    }
    out += '}';
    return out;
}

} // namespace chat
