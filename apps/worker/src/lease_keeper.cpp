#include "lease_keeper.hpp"

#include "heartbeat.hpp"
#include "job_runner.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <stop_token>
#include <utility>

namespace worker {

LeaseKeeper::LeaseKeeper(core::ports::IJobQueue& queue, ops::Logger& log, const core::NodeId& node,
                         const core::ports::JobLease& lease, const Intervals& intervals,
                         std::stop_source abandon, const Heartbeat* heartbeat)
    : queue_(queue), log_(log), node_(node), lease_(lease), intervals_(intervals),
      abandon_(std::move(abandon)), heartbeat_(heartbeat),
      thread_([this](const std::stop_token& stop) { run(stop); }) {}

LeaseKeeper::~LeaseKeeper() = default;

void LeaseKeeper::report(std::uint8_t percent) noexcept {
    percent_.store(percent);
}

void LeaseKeeper::lose(std::string_view call) {
    lost_.store(true);
    abandon_.request_stop();
    log_.warn("fenced out",
              {{"job", std::to_underlying(lease_.job)}, {"call", call}, {"fence", lease_.fence}});
}

bool LeaseKeeper::write_progress() {
    const std::uint8_t percent = percent_.load();
    if (written_ == percent) {
        return true;
    }
    const auto r = queue_.report_progress(lease_, percent);
    if (r && !*r) {
        lose("progress");
        return false;
    }
    // A refused value would be refused again at every tick; it is reported once and let go.
    if (!r && r.error() != core::ports::JobQueueError::Unavailable) {
        log_.error("job queue call failed", {{"job", std::to_underlying(lease_.job)},
                                             {"call", "progress"},
                                             {"error", to_string(r.error())}});
    }
    if (r || r.error() != core::ports::JobQueueError::Unavailable) {
        written_ = percent;
    }
    return true;
}

bool LeaseKeeper::beat() {
    // Touched whatever the database says: the process is alive and beating, and an unreachable
    // database is no reason for the kubelet to restart it.
    if (heartbeat_ != nullptr) {
        heartbeat_->beat();
    }
    const auto r = queue_.heartbeat(lease_, node_);
    if (r && !*r) {
        lose("heartbeat");
        return false;
    }
    // An unreachable database is retried at the next beat; if it stays away past the lease,
    // another worker's claim fences us out and the next beat says so.
    if (!r) {
        log_.log(r.error() == core::ports::JobQueueError::Unavailable ? ops::Level::Warn
                                                                      : ops::Level::Error,
                 "job queue call failed",
                 {{"job", std::to_underlying(lease_.job)},
                  {"call", "heartbeat"},
                  {"error", to_string(r.error())}});
    }
    return true;
}

// Waits use the steady clock itself: a condition variable cannot wait on an injected clock,
// and the tests shorten the intervals instead. A process stopped past a deadline beats as
// soon as it runs again, which is when a zombie must find out it lost the lease.
void LeaseKeeper::run(const std::stop_token& stop) {
    using Clock = std::chrono::steady_clock;
    // Taking the mutex before notifying means the stop cannot land between the wait's check of
    // the predicate and its sleep.
    const std::stop_callback wake_on_stop(stop, [this] {
        { const std::scoped_lock lock(mutex_); }
        wake_.notify_one();
    });
    auto next_beat = Clock::now() + intervals_.heartbeat;
    auto next_progress = Clock::now() + intervals_.progress;
    while (true) {
        {
            std::unique_lock lock(mutex_);
            wake_.wait_until(lock, std::min(next_beat, next_progress),
                             [&stop] { return stop.stop_requested(); });
        }
        if (stop.stop_requested()) {
            return;
        }
        const auto now = Clock::now();
        if (now >= next_progress) {
            next_progress = now + intervals_.progress;
            if (!write_progress()) {
                return;
            }
        }
        if (now >= next_beat) {
            next_beat = now + intervals_.heartbeat;
            if (!beat()) {
                return;
            }
        }
    }
}

} // namespace worker
