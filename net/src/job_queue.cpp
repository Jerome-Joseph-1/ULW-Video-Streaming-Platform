#include "job_queue.hpp"

namespace net::detail {

void JobQueue::push(IOffloadJob& job) {
    {
        const std::scoped_lock lock(mutex_);
        jobs_.push_back(&job);
    }
    ready_.notify_one();
}

IOffloadJob* JobQueue::pop(const std::stop_token& stop) {
    std::unique_lock lock(mutex_);
    // Once stop is requested wait() returns whether jobs remain, not whether to take one.
    ready_.wait(lock, stop, [this] { return !jobs_.empty(); });
    if (stop.stop_requested()) {
        return nullptr;
    }
    IOffloadJob* job = jobs_.front();
    jobs_.pop_front();
    return job;
}

} // namespace net::detail
