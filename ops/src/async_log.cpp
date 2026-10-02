#include "ops/async_log.hpp"

#include <sys/eventfd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <poll.h>
#include <unistd.h>
#include <utility>

namespace ops {

std::expected<std::unique_ptr<AsyncLogSink>, int> AsyncLogSink::create(int fd, std::size_t capacity,
                                                                       core::Millis flush_limit) {
    os::UniqueFd stopping(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (!stopping) {
        return std::unexpected(errno);
    }
    return std::make_unique<AsyncLogSink>(Key{}, fd, capacity, flush_limit, std::move(stopping));
}

AsyncLogSink::AsyncLogSink(Key /*key*/, int fd, std::size_t capacity, core::Millis flush_limit,
                           os::UniqueFd stopping)
    : fd_(fd), capacity_(capacity), flush_limit_(flush_limit), stopping_(std::move(stopping)) {
    // Both buffers are sized once, so appending a line never allocates.
    queued_.reserve(capacity_);
    out_.reserve(capacity_);
    thread_ = std::jthread([this](const std::stop_token& stop) { drain(stop); });
}

AsyncLogSink::~AsyncLogSink() {
    static_cast<void>(close());
}

std::uint64_t AsyncLogSink::close() noexcept {
    if (!closed_.exchange(true)) {
        thread_.request_stop();
        const std::uint64_t one = 1;
        // An eventfd write only fails when its counter would overflow, which one write cannot do.
        [[maybe_unused]] const ssize_t woken = ::write(stopping_.get(), &one, sizeof one);
        thread_.join();
    }
    return dropped();
}

void AsyncLogSink::write(std::string_view line) noexcept {
    bool was_empty = false;
    {
        const std::scoped_lock lock(mutex_);
        if (closed_.load() || queued_.size() + line.size() > capacity_) {
            dropped_.fetch_add(1);
            return;
        }
        was_empty = queued_.empty();
        queued_.insert(queued_.end(), line.begin(), line.end());
    }
    if (was_empty) {
        wake_.notify_one();
    }
}

// Waits for the reader with poll() rather than in write(), so the wait can end: when the sink is
// destroyed, the eventfd wakes it and the flush deadline starts. False once that deadline has
// passed or the destination is gone.
bool AsyncLogSink::wait_writable(std::optional<core::MonoTime>& deadline,
                                 const std::stop_token& stop) const {
    while (true) {
        if (!deadline && stop.stop_requested()) {
            deadline = std::chrono::steady_clock::now() + flush_limit_;
        }
        int timeout = -1;
        if (deadline) {
            const auto left = std::chrono::duration_cast<core::Millis>(
                *deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) {
                return false;
            }
            timeout = static_cast<int>(std::min<core::Millis::rep>(left.count(), INT_MAX));
        }
        std::array<pollfd, 2> fds{{{.fd = fd_, .events = POLLOUT, .revents = 0},
                                   {.fd = stopping_.get(), .events = POLLIN, .revents = 0}}};
        const int ready = ::poll(fds.data(), deadline ? 1 : 2, timeout);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready < 0 || (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return false;
        }
        if ((fds[0].revents & POLLOUT) != 0) {
            return true;
        }
    }
}

// At most PIPE_BUF at a time, which a pipe that polls writable takes without blocking.
bool AsyncLogSink::write_out(std::optional<core::MonoTime>& deadline, const std::stop_token& stop) {
    std::string_view rest(out_.data(), out_.size());
    while (!rest.empty() && wait_writable(deadline, stop)) {
        const ssize_t n = ::write(fd_, rest.data(), std::min<std::size_t>(rest.size(), PIPE_BUF));
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        rest.remove_prefix(static_cast<std::size_t>(n));
    }
    if (rest.empty()) {
        return true;
    }
    dropped_.fetch_add(static_cast<std::uint64_t>(std::ranges::count(rest, '\n')));
    return false;
}

void AsyncLogSink::drain(const std::stop_token& stop) {
    std::optional<core::MonoTime> deadline;
    while (true) {
        {
            std::unique_lock lock(mutex_);
            // The stop is only honoured once nothing is queued, so the destructor flushes.
            wake_.wait(lock, stop, [this] { return !queued_.empty(); });
            if (queued_.empty()) {
                return;
            }
            queued_.swap(out_);
        }
        const bool whole = write_out(deadline, stop);
        out_.clear();
        if (!whole) {
            // The reader stalled past the deadline; what is still queued goes the same way.
            const std::scoped_lock lock(mutex_);
            dropped_.fetch_add(static_cast<std::uint64_t>(std::ranges::count(queued_, '\n')));
            queued_.clear();
            if (stop.stop_requested()) {
                return;
            }
        }
    }
}

} // namespace ops
