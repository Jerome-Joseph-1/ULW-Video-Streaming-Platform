#include "worker.hpp"

#include "log.hpp"

#include <optional>

namespace worker {

void run_worker(core::ports::IJobQueue& queue, JobRunner& runner, const core::NodeId& node,
                const std::stop_token& shutdown) {
    while (!shutdown.stop_requested()) {
        if (const auto reaped = queue.reap_expired(); reaped && *reaped > 0) {
            log("requeued {} jobs whose lease lapsed", *reaped);
        }
        const auto claimed = queue.claim(node);
        if (!claimed) {
            log("claim: the job queue call failed");
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
