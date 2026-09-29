#include "worker.hpp"

#include <optional>

namespace worker {

void run_worker(core::ports::IJobQueue& queue, JobRunner& runner, const core::NodeId& node,
                const Heartbeat& heartbeat, ops::Logger& log, const std::stop_token& shutdown) {
    while (!shutdown.stop_requested()) {
        heartbeat.beat();
        if (const auto reaped = queue.reap_expired(); reaped && *reaped > 0) {
            log.info("requeued lapsed jobs", {{"count", *reaped}});
        }
        const auto claimed = queue.claim(node);
        if (!claimed) {
            // An outage passes; a refused claim statement is a bug that polling will not fix,
            // but a worker that exits would only be restarted into the same refusal.
            log.log(claimed.error() == core::ports::JobQueueError::Unavailable ? ops::Level::Warn
                                                                               : ops::Level::Error,
                    "job queue call failed",
                    {{"call", "claim"}, {"error", to_string(claimed.error())}});
            queue.wait_for_work(kPollInterval);
            continue;
        }
        const std::optional<core::ports::ClaimedJob>& job = *claimed;
        if (!job) {
            queue.wait_for_work(kPollInterval);
            continue;
        }
        [[maybe_unused]] const JobOutcome outcome = runner.run(*job, shutdown);
    }
}

} // namespace worker
