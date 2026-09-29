#pragma once

#include "ops/log.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace ops {

// Lines from the event loop go through here: a write() to a pipe whose reader has stalled
// (journald under load, a full disk behind a file) would stop the loop, so lines are copied
// into a bounded buffer and written out by a thread of their own. When the buffer is full the
// line is dropped and counted; the caller never waits for the destination.
class AsyncLogSink final : public ILogSink {
public:
    // `fd` is borrowed and must outlive the sink. `capacity` bytes are held at most while the
    // thread writes out as many more.
    AsyncLogSink(int fd, std::size_t capacity);
    // Writes out everything queued before returning.
    ~AsyncLogSink() override;
    AsyncLogSink(const AsyncLogSink&) = delete;
    AsyncLogSink& operator=(const AsyncLogSink&) = delete;
    AsyncLogSink(AsyncLogSink&&) = delete;
    AsyncLogSink& operator=(AsyncLogSink&&) = delete;

    void write(std::string_view line) noexcept override;
    [[nodiscard]] std::uint64_t dropped() const noexcept override {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    void drain(const std::stop_token& stop);

    int fd_;
    std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    // Filled by writers under the mutex; swapped with `out_` by the thread, which then writes
    // `out_` without holding it.
    std::vector<char> queued_;
    std::vector<char> out_;
    std::atomic<std::uint64_t> dropped_{0};
    std::jthread thread_;
};

} // namespace ops
