#include "view_recorder.hpp"

#include <utility>

namespace gateway {

ViewRecorder::ViewRecorder(net::IReactor& reactor, core::ports::IViewLog& log, std::size_t capacity,
                           core::Millis interval)
    : reactor_(reactor), log_(log), capacity_(capacity), interval_(interval) {
    pending_.reserve(capacity_);
}

ViewRecorder::~ViewRecorder() {
    if (armed_) {
        reactor_.cancel_timer(timer_);
    }
}

void ViewRecorder::record(const core::ports::ViewEvent& event) noexcept {
    if (pending_.size() >= capacity_) {
        ++counters_.dropped;
        return;
    }
    pending_.push_back(event);
    arm();
}

void ViewRecorder::arm() noexcept {
    if (!armed_) {
        timer_ = reactor_.arm_timer(interval_, *this);
        armed_ = true;
    }
}

void ViewRecorder::on_timeout() noexcept {
    armed_ = false;
    flush();
}

void ViewRecorder::flush() noexcept {
    if (pending_.empty()) {
        return;
    }
    if (writing_) {
        // One write at a time; this one follows as soon as the write in flight ends.
        hurry_ = true;
        return;
    }
    std::vector<core::ports::ViewEvent> batch = std::exchange(pending_, {});
    // Once per batch, not per event: recording itself stays allocation-free.
    pending_.reserve(capacity_);
    const std::size_t n = batch.size();
    writing_ = true;
    log_.record_views(std::move(batch), [this, n](core::ports::CatalogResult<void> r) noexcept {
        writing_ = false;
        if (r) {
            counters_.recorded += n;
        } else {
            ++counters_.failed_batches;
            counters_.dropped += n;
        }
        if (std::exchange(hurry_, false)) {
            flush();
        } else if (!pending_.empty()) {
            arm();
        }
    });
}

} // namespace gateway
