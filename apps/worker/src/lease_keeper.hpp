#pragma once

#include "core/models/ids.hpp"
#include "core/ports/job_queue.hpp"
#include "core/util/time.hpp"

#include "ops/log.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string_view>
#include <thread>

namespace worker {

class Heartbeat;

// Keeps one job's lease alive from a thread of its own while the job runs, and writes its
// progress. When the queue says the lease is gone, it fires `abandon`, and the job must stop
// without writing anything more.
class LeaseKeeper {
public:
    struct Intervals {
        core::Millis heartbeat = core::ports::kJobHeartbeat;
        // The spec's "about every 10 s": the progress bar moves, the jobs row is not churned.
        core::Millis progress{10'000};
    };

    // `queue` is used from the keeper's thread only, so it must not be the one the job uses.
    // `heartbeat`, when there is one, is touched on every beat.
    LeaseKeeper(core::ports::IJobQueue& queue, ops::Logger& log, const core::NodeId& node,
                const core::ports::JobLease& lease, const Intervals& intervals,
                std::stop_source abandon, const Heartbeat* heartbeat = nullptr);
    ~LeaseKeeper();
    LeaseKeeper(const LeaseKeeper&) = delete;
    LeaseKeeper& operator=(const LeaseKeeper&) = delete;
    LeaseKeeper(LeaseKeeper&&) = delete;
    LeaseKeeper& operator=(LeaseKeeper&&) = delete;

    // Written on the next progress tick if it changed.
    void report(std::uint8_t percent) noexcept;
    [[nodiscard]] bool lost() const noexcept { return lost_.load(); }

private:
    void run(const std::stop_token& stop);
    // Each false when the queue refused the lease.
    [[nodiscard]] bool write_progress();
    [[nodiscard]] bool beat();
    void lose(std::string_view call);

    core::ports::IJobQueue& queue_;
    ops::Logger& log_;
    core::NodeId node_;
    core::ports::JobLease lease_;
    Intervals intervals_;
    std::stop_source abandon_;
    const Heartbeat* heartbeat_;
    std::atomic<std::uint8_t> percent_{0};
    std::atomic<bool> lost_{false};
    // What the jobs row last took; the keeper's thread alone touches it.
    std::optional<std::uint8_t> written_;
    std::mutex mutex_;
    std::condition_variable wake_;
    // Last: the thread must stop before anything it uses is destroyed.
    std::jthread thread_;
};

} // namespace worker
