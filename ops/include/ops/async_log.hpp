#pragma once

#include "core/util/time.hpp"
#include "os/unique_fd.hpp"

#include "ops/log.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
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
    // Lets only create() construct, while make_unique can still reach the constructor.
    class Key {
        friend class AsyncLogSink;
        Key() = default;
    };

    // `fd` is borrowed and must outlive the sink. `capacity` bytes are held at most while the
    // thread writes out as many more. `flush_limit` bounds how long close() waits for a reader
    // to take what is still queued. Fails with the errno of the eventfd that close() needs to
    // wake a thread waiting on a stalled reader: without it that wait, and so the exit, would
    // never end.
    [[nodiscard]] static std::expected<std::unique_ptr<AsyncLogSink>, int>
    create(int fd, std::size_t capacity, core::Millis flush_limit);

    AsyncLogSink(Key key, int fd, std::size_t capacity, core::Millis flush_limit,
                 os::UniqueFd stopping);
    // Closes the sink if close() was not called.
    ~AsyncLogSink() override;
    AsyncLogSink(const AsyncLogSink&) = delete;
    AsyncLogSink& operator=(const AsyncLogSink&) = delete;
    AsyncLogSink(AsyncLogSink&&) = delete;
    AsyncLogSink& operator=(AsyncLogSink&&) = delete;

    // Writes out what is queued, for at most the flush limit, and stops the thread; what a
    // stalled reader leaves after that is dropped and counted. Later writes are dropped too.
    // Returns the lines dropped over the sink's life, which by now nothing else can report.
    [[nodiscard]] std::uint64_t close() noexcept;

    void write(std::string_view line) noexcept override;
    [[nodiscard]] std::uint64_t dropped() const noexcept override { return dropped_.load(); }

private:
    void drain(const std::stop_token& stop);
    [[nodiscard]] bool wait_writable(std::optional<core::MonoTime>& deadline,
                                     const std::stop_token& stop) const;
    // Writes `out_` out; false when the flush deadline passed first.
    bool write_out(std::optional<core::MonoTime>& deadline, const std::stop_token& stop);

    int fd_;
    std::size_t capacity_;
    core::Millis flush_limit_;
    // Wakes the thread from a wait for the reader when the sink is being destroyed.
    os::UniqueFd stopping_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    // Filled by writers under the mutex; swapped with `out_` by the thread, which then writes
    // `out_` without holding it.
    std::vector<char> queued_;
    std::vector<char> out_;
    std::atomic<std::uint64_t> dropped_{0};
    // Set by close(); a line written after it would sit in a buffer no thread writes out.
    std::atomic<bool> closed_{false};
    std::jthread thread_;
};

} // namespace ops
