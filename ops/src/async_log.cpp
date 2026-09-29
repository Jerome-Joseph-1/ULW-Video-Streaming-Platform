#include "ops/async_log.hpp"

#include <cerrno>
#include <unistd.h>

namespace ops {

AsyncLogSink::AsyncLogSink(int fd, std::size_t capacity) : fd_(fd), capacity_(capacity) {
    // Both buffers are sized once, so appending a line never allocates.
    queued_.reserve(capacity_);
    out_.reserve(capacity_);
    thread_ = std::jthread([this](const std::stop_token& stop) { drain(stop); });
}

AsyncLogSink::~AsyncLogSink() {
    thread_.request_stop();
    thread_.join();
}

void AsyncLogSink::write(std::string_view line) noexcept {
    bool was_empty = false;
    {
        const std::scoped_lock lock(mutex_);
        if (queued_.size() + line.size() > capacity_) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        was_empty = queued_.empty();
        queued_.insert(queued_.end(), line.begin(), line.end());
    }
    if (was_empty) {
        wake_.notify_one();
    }
}

void AsyncLogSink::drain(const std::stop_token& stop) {
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
        std::string_view rest(out_.data(), out_.size());
        while (!rest.empty()) {
            const ssize_t n = ::write(fd_, rest.data(), rest.size());
            if (n < 0 && errno == EINTR) {
                continue;
            }
            // A destination that is gone takes nothing more; the lines are lost either way.
            if (n <= 0) {
                break;
            }
            rest.remove_prefix(static_cast<std::size_t>(n));
        }
        out_.clear();
    }
}

} // namespace ops
