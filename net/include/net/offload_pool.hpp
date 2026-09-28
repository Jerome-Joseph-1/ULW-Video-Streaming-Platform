#pragma once

#include "net/reactor.hpp"

#include <cstddef>
#include <expected>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace net {

namespace detail {
class JobQueue;
} // namespace detail

// Work that must not run on the reactor thread: blocking storage control calls, file I/O.
class IOffloadJob {
public:
    virtual ~IOffloadJob() = default;
    // On a pool thread. May block.
    virtual void run() noexcept = 0;
    // Back on the reactor thread, once run() has returned.
    virtual void complete() noexcept = 0;
};

// A fixed set of threads fed from the reactor thread. Completions come back through an
// eventfd the reactor watches, so complete() always runs on the loop, never concurrently
// with a handler. A submitted job must stay alive until its complete() has run or the pool
// has been destroyed.
class OffloadPool final : public IReadyHandler {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<OffloadPool>, int>
    create(IReactor& reactor, std::size_t threads);

    OffloadPool(IReactor& reactor, os::UniqueFd event_fd, std::size_t threads);
    // Stops the threads. A job already running finishes first; jobs still queued never run
    // and never complete.
    ~OffloadPool() override;
    OffloadPool(const OffloadPool&) = delete;
    OffloadPool& operator=(const OffloadPool&) = delete;

    void submit(IOffloadJob& job);
    // Jobs submitted whose complete() has not yet run.
    [[nodiscard]] std::size_t in_flight() const noexcept { return in_flight_; }

    void on_ready(Interest ready) noexcept override;

private:
    void worker(const std::stop_token& stop) noexcept;

    IReactor& reactor_;
    os::UniqueFd event_fd_;
    std::size_t in_flight_ = 0;

    std::unique_ptr<detail::JobQueue> queue_;

    std::mutex done_mutex_;
    std::vector<IOffloadJob*> done_;
    std::vector<IOffloadJob*> completing_;

    // Last member: the threads must stop before anything they touch is destroyed.
    std::vector<std::jthread> threads_;
};

} // namespace net
