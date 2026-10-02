#include "job_queue.hpp"

#include <mutex>

namespace net::detail {

void JobQueue::push(IOffloadJob& job) {
    {
        const std::scoped_lock lock(mutex_);
        jobs_.push_back(&job);
    }
    ready_.notify_one();
}

void JobQueue::wake_all() {
    { const std::scoped_lock lock(mutex_); }
    ready_.notify_all();
}

IOffloadJob* JobQueue::pop(const std::stop_token& stop) {
    std::unique_lock lock(mutex_);
    ready_.wait(lock, [this, &stop] { return !jobs_.empty() || stop.stop_requested(); });
    if (stop.stop_requested()) {
        return nullptr;
    }
    IOffloadJob* job = jobs_.front();
    jobs_.pop_front();
    return job;
}

} // namespace net::detail
