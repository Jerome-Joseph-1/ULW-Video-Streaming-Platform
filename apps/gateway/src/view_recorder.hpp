#pragma once

#include "core/ports/views.hpp"
#include "net/reactor.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gateway {

struct ViewCounters {
    std::uint64_t recorded = 0;
    // Refused because the batch was full, or lost with a batch the log failed to write.
    std::uint64_t dropped = 0;
    std::uint64_t failed_batches = 0;
};

// Collects view events and writes them in batches on a timer, one write in flight at a time.
// Recording never waits and never allocates: past `capacity` pending events, an event is
// counted and dropped, so a slow or dead database costs analytics, never playback.
// Everything runs on the reactor thread.
class ViewRecorder final : public net::ITimerHandler {
public:
    ViewRecorder(net::IReactor& reactor, core::ports::IViewLog& log, std::size_t capacity,
                 core::Millis interval);
    ~ViewRecorder() override;
    ViewRecorder(const ViewRecorder&) = delete;
    ViewRecorder& operator=(const ViewRecorder&) = delete;
    ViewRecorder(ViewRecorder&&) = delete;
    ViewRecorder& operator=(ViewRecorder&&) = delete;

    void record(const core::ports::ViewEvent& event) noexcept;
    // Writes what is pending now, and from here on writes each event as it arrives, so the
    // requests a drain lets finish do not hold the exit for a whole interval.
    void drain() noexcept;
    [[nodiscard]] bool idle() const noexcept { return !writing_ && pending_.empty(); }
    [[nodiscard]] const ViewCounters& counters() const noexcept { return counters_; }

    void on_timeout() noexcept override;

private:
    void arm() noexcept;
    void flush() noexcept;

    net::IReactor& reactor_;
    core::ports::IViewLog& log_;
    std::size_t capacity_;
    core::Millis interval_;
    std::vector<core::ports::ViewEvent> pending_;
    net::TimerId timer_;
    bool armed_ = false;
    bool writing_ = false;
    bool hurry_ = false;
    bool draining_ = false;
    ViewCounters counters_;
};

} // namespace gateway
