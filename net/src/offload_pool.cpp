#include "net/offload_pool.hpp"

#include "job_queue.hpp"

#include <sys/eventfd.h>

#include <cerrno>
#include <cstdint>
#include <stop_token>
#include <unistd.h>

namespace net {

std::expected<std::unique_ptr<OffloadPool>, int> OffloadPool::create(IReactor& reactor,
                                                                     std::size_t threads) {
    os::UniqueFd efd{::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)};
    if (!efd) {
        return std::unexpected(errno);
    }
    const int raw = efd.get();
    auto pool = std::make_unique<OffloadPool>(reactor, std::move(efd), threads);
    if (auto r = reactor.watch(raw, Interest::Read, *pool); !r) {
        return std::unexpected(r.error());
    }
    return pool;
}

OffloadPool::OffloadPool(IReactor& reactor, os::UniqueFd event_fd, std::size_t threads)
    : reactor_(reactor), event_fd_(std::move(event_fd)),
      queue_(std::make_unique<detail::JobQueue>()) {
    threads_.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
        threads_.emplace_back([this](const std::stop_token& stop) { worker(stop); });
    }
}

OffloadPool::~OffloadPool() {
    for (auto& t : threads_) {
        t.request_stop();
    }
    threads_.clear();
    reactor_.unwatch(event_fd_.get());
}

void OffloadPool::submit(IOffloadJob& job) {
    ++in_flight_;
    queue_->push(job);
}

void OffloadPool::worker(const std::stop_token& stop) noexcept {
    // Once per thread, for as long as it pops: a stop wakes every waiting worker.
    const std::stop_callback wake_on_stop(stop, [this] { queue_->wake_all(); });
    while (IOffloadJob* job = queue_->pop(stop)) {
        job->run();
        {
            const std::scoped_lock lock(done_mutex_);
            done_.push_back(job);
        }
        // Pushed before the write, and on_ready reads before it takes the list, so no job can
        // be left behind without a wakeup still pending for it. The counter cannot overflow
        // (2^64 - 2 pending wakeups), so the write cannot fail.
        const std::uint64_t one = 1;
        [[maybe_unused]] const ssize_t written = ::write(event_fd_.get(), &one, sizeof one);
    }
}

void OffloadPool::on_ready(Interest /*ready*/) noexcept {
    std::uint64_t count = 0;
    // Resets the counter; an empty read only means another wakeup already took the list.
    [[maybe_unused]] const ssize_t got = ::read(event_fd_.get(), &count, sizeof count);
    {
        const std::scoped_lock lock(done_mutex_);
        completing_.swap(done_);
    }
    for (IOffloadJob* job : completing_) {
        --in_flight_;
        job->complete();
    }
    completing_.clear();
}

} // namespace net
