#pragma once

#include "core/util/time.hpp"
#include "os/unique_fd.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace net {

// Valid only for the duration of the callback that receives it.
using BorrowedBytes = std::span<const std::byte>;

enum class Interest : std::uint8_t { None = 0, Read = 1, Write = 2, ReadWrite = 3 };

[[nodiscard]] constexpr bool has(Interest set, Interest bit) noexcept {
    return (static_cast<std::uint8_t>(set) & static_cast<std::uint8_t>(bit)) != 0;
}

[[nodiscard]] constexpr Interest operator|(Interest a, Interest b) noexcept {
    return static_cast<Interest>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

// A connection owned by the reactor. The generation turns a handle kept past close into a
// no-op instead of an operation on whichever connection reused the descriptor number.
struct ConnId {
    std::int32_t fd = -1;
    std::uint32_t gen = 0;
    friend bool operator==(ConnId, ConnId) = default;
};

struct TimerId {
    std::uint32_t index = 0;
    std::uint32_t gen = 0;
    friend bool operator==(TimerId, TimerId) = default;
};

// Mode A: the reactor owns the socket and hands over bytes it has already read.
class IStreamHandler {
public:
    virtual ~IStreamHandler() = default;
    virtual void on_data(BorrowedBytes bytes) noexcept = 0;
    // The send queue drained. Only meaningful if pending_send_bytes() was non-zero after the
    // last send(): a reactor may write synchronously and skip the callback.
    virtual void on_writable() noexcept = 0;
    virtual void on_peer_eof() noexcept = 0;
    virtual void on_error(int err) noexcept = 0;
};

// Mode B: readiness for a descriptor some library owns (libcurl, libpq, signalfd).
class IReadyHandler {
public:
    virtual ~IReadyHandler() = default;
    virtual void on_ready(Interest ready) noexcept = 0;
};

class ITimerHandler {
public:
    virtual ~ITimerHandler() = default;
    virtual void on_timeout() noexcept = 0;
};

class IAcceptHandler {
public:
    virtual ~IAcceptHandler() = default;
    // The socket is nonblocking and close-on-exec.
    virtual void on_accept(os::UniqueFd conn) noexcept = 0;
};

// Single-threaded: every member must be called on the thread that runs run_once(), and
// every callback runs on it. Handlers may call back into the reactor from any callback.
class IReactor {
public:
    virtual ~IReactor() = default;

    [[nodiscard]] virtual std::expected<void, int> listen(os::UniqueFd listener,
                                                          IAcceptHandler& handler) = 0;
    virtual void stop_listening() noexcept = 0;

    // The connection starts out not receiving.
    [[nodiscard]] virtual std::expected<ConnId, int> attach(os::UniqueFd conn,
                                                            IStreamHandler& handler) = 0;
    virtual void start_receiving(ConnId conn) noexcept = 0;
    // Exact: no on_data follows until start_receiving. Unread bytes wait in the kernel's
    // receive buffer, which is where backpressure belongs.
    virtual void stop_receiving(ConnId conn) noexcept = 0;
    // Copies `bytes` into the connection's send queue; partial writes are the reactor's job.
    virtual void send(ConnId conn, std::span<const std::byte> bytes) noexcept = 0;
    [[nodiscard]] virtual std::size_t pending_send_bytes(ConnId conn) const noexcept = 0;
    // Drops unsent bytes, cancels in-flight work and closes the socket once the kernel holds
    // no reference to it. No callback for this connection follows.
    virtual void begin_close(ConnId conn) noexcept = 0;
    [[nodiscard]] virtual bool is_quiescent(ConnId conn) const noexcept = 0;

    // The caller keeps ownership of `fd` and must unwatch() before closing it.
    [[nodiscard]] virtual std::expected<void, int> watch(int fd, Interest interest,
                                                         IReadyHandler& handler) = 0;
    virtual void unwatch(int fd) noexcept = 0;

    virtual TimerId arm_timer(core::Millis delay, ITimerHandler& handler) = 0;
    virtual void cancel_timer(TimerId timer) noexcept = 0;

    // Sampled once per iteration, before dispatch.
    [[nodiscard]] virtual core::MonoTime now() const noexcept = 0;
    // Waits at most `max_wait`, dispatches, fires due timers. Returns events dispatched.
    virtual int run_once(core::Millis max_wait) = 0;
};

} // namespace net
