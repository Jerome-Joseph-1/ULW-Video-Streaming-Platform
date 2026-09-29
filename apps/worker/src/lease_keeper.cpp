#include "lease_keeper.hpp"

#include "heartbeat.hpp"
#include "log.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace worker {

LeaseKeeper::LeaseKeeper(core::ports::IJobQueue& queue, const core::NodeId& node,
                         const core::ports::JobLease& lease, const Intervals& intervals,
                         std::stop_source abandon, const Heartbeat* heartbeat)
    : queue_(queue), node_(node), lease_(lease), intervals_(intervals),
      abandon_(std::move(abandon)), heartbeat_(heartbeat),
      thread_([this](const std::stop_token& stop) { run(stop); }) {}

LeaseKeeper::~LeaseKeeper() = default;

void LeaseKeeper::report(std::uint8_t percent) noexcept {
    percent_.store(percent);
}

void LeaseKeeper::lose(const char* how) {
    lost_.store(true);
    abandon_.request_stop();
    log("job={} lease lost: {} with fence {} matched no row", std::to_underlying(lease_.job), how,
        lease_.fence);
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
    if (r) {
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
        log("job={} heartbeat: the job queue call failed", std::to_underlying(lease_.job));
    }
    return true;
}

// Waits use the steady clock itself: a condition variable cannot wait on an injected clock,
// and the tests shorten the intervals instead. A process stopped past a deadline beats as
// soon as it runs again, which is when a zombie must find out it lost the lease.
void LeaseKeeper::run(const std::stop_token& stop) {
    using Clock = std::chrono::steady_clock;
    auto next_beat = Clock::now() + intervals_.heartbeat;
    auto next_progress = Clock::now() + intervals_.progress;
    while (true) {
        {
            std::unique_lock lock(mutex_);
            wake_.wait_until(lock, stop, std::min(next_beat, next_progress), [] { return false; });
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
