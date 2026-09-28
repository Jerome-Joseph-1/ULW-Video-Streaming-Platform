#pragma once

#include "net/offload_pool.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <stop_token>

namespace net::detail {

// Hands jobs from the reactor thread to the pool's workers.
class JobQueue {
public:
    void push(IOffloadJob& job);
    // Blocks until a job is queued. Returns nullptr once `stop` is requested, even with jobs
    // still queued: those are never handed out.
    [[nodiscard]] IOffloadJob* pop(const std::stop_token& stop);

private:
    std::mutex mutex_;
    std::condition_variable_any ready_;
    std::deque<IOffloadJob*> jobs_;
};

} // namespace net::detail
