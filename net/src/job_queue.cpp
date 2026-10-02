#include "job_queue.hpp"

#include <mutex>
#include <stop_token>

namespace net::detail {

void JobQueue::push(IOffloadJob& job) {
    {
        const std::scoped_lock lock(mutex_);
        jobs_.push_back(&job);
    }
    ready_.notify_one();
}

IOffloadJob* JobQueue::pop(const std::stop_token& stop) {
    // Taking the mutex before notifying means the stop cannot land between the wait's check of
    // the predicate and its sleep. Each popping worker has its own callback, so all of them wake.
    const std::stop_callback wake_on_stop(stop, [this] {
        { const std::scoped_lock lock(mutex_); }
        ready_.notify_all();
    });
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
