#pragma once

#include "core/ports/job_queue.hpp"

#include <memory>
#include <string>

namespace infra::postgres {

// IJobQueue on Postgres (ADR-0006). One session per queue: it runs the statements and LISTENs
// for job_available. It connects on first use and reconnects on the call after a failure.
class PgJobQueue final : public core::ports::IJobQueue {
public:
    explicit PgJobQueue(std::string conninfo);
    ~PgJobQueue() override;
    PgJobQueue(const PgJobQueue&) = delete;
    PgJobQueue& operator=(const PgJobQueue&) = delete;
    PgJobQueue(PgJobQueue&&) = delete;
    PgJobQueue& operator=(PgJobQueue&&) = delete;

    [[nodiscard]] core::ports::JobQueueResult<std::optional<core::ports::ClaimedJob>>
    claim(const core::NodeId& worker) override;
    [[nodiscard]] core::ports::JobQueueResult<bool> heartbeat(const core::ports::JobLease& lease,
                                                              const core::NodeId& worker) override;
    [[nodiscard]] core::ports::JobQueueResult<bool>
    report_progress(const core::ports::JobLease& lease, std::uint8_t percent) override;
    [[nodiscard]] core::ports::JobQueueResult<bool>
    finish(const core::ports::JobLease& lease, core::Millis duration,
           std::span<const core::ports::Rendition> renditions) override;
    [[nodiscard]] core::ports::JobQueueResult<bool>
    fail(const core::ports::JobLease& lease, std::string_view reason, bool retryable) override;
    [[nodiscard]] core::ports::JobQueueResult<std::size_t> reap_expired() override;
    void wait_for_work(core::Millis max_wait) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace infra::postgres
