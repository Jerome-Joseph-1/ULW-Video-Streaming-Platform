#pragma once

#include "core/models/ids.hpp"
#include "core/ports/job_queue.hpp"

#include "heartbeat.hpp"
#include "job_runner.hpp"

#include <stop_token>

namespace worker {

// ADR-0006: a wakeup is only a hint, so the queue is polled this often regardless.
inline constexpr core::Millis kPollInterval{5000};

// Claims jobs one at a time and runs each to completion until `shutdown` fires. Between jobs
// it returns lapsed leases to the queue: nothing else runs the reaper. Every pass touches
// `heartbeat`.
void run_worker(core::ports::IJobQueue& queue, JobRunner& runner, const core::NodeId& node,
                const Heartbeat& heartbeat, const std::stop_token& shutdown);

} // namespace worker
