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
    // still queued: those are never handed out. The caller wakes it on a stop: a
    // std::stop_callback on `stop` that calls wake_all(), registered once per popping thread
    // before its first pop.
    [[nodiscard]] IOffloadJob* pop(const std::stop_token& stop);
    // Wakes every pop() so each checks its stop token again. It takes the mutex before
    // notifying, so a stop cannot land between a pop's check of its token and its sleep.
    void wake_all();

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<IOffloadJob*> jobs_;
};

} // namespace net::detail
