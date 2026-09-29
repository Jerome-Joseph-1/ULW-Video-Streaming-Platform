#pragma once

#include "core/util/time.hpp"
#include "net/socket_addr.hpp"
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

// A UDP socket owned by the reactor. The generation plays the same part as in ConnId.
struct DatagramId {
    std::int32_t fd = -1;
    std::uint32_t gen = 0;
    friend bool operator==(DatagramId, DatagramId) = default;
};

// The largest payload either direction carries. Media on the internet is sized to the 1500-byte
// Ethernet MTU, which leaves at most 1500 - 20 (IPv4) - 8 (UDP) = 1472 bytes; 2 KiB is the next
// power of two. A longer datagram is dropped and counted as truncated rather than delivered cut.
inline constexpr std::size_t kMaxDatagramSize = 2048;

struct DatagramStats {
    std::uint64_t received = 0;
    // Longer than kMaxDatagramSize.
    std::uint64_t truncated = 0;
    // io_uring only: a datagram arrived while every receive buffer was in use and waited in the
    // socket until one came back.
    std::uint64_t ring_exhausted = 0;
    // io_uring only: arrived after a stop, beyond what one stopped socket may keep in the shared
    // receive buffers, and dropped as a full receive buffer would have dropped it.
    std::uint64_t stopped_drops = 0;
    std::uint64_t sent = 0;
    // io_uring only: sends that went out as SENDMSG_ZC, and notifications of those that said the
    // kernel copied the payload anyway.
    std::uint64_t zero_copy_sends = 0;
    std::uint64_t zero_copy_copied = 0;
    std::uint64_t send_refused = 0;
    std::uint64_t send_errors = 0;
};

// Mode C: the reactor owns a UDP socket and hands over each datagram it has already read.
class IDatagramHandler {
public:
    virtual ~IDatagramHandler() = default;
    virtual void on_datagram(SocketAddr from, BorrowedBytes payload) noexcept = 0;
    // The kernel refused a datagram send_to had accepted (ENETUNREACH, EACCES, ...).
    virtual void on_send_error(SocketAddr to, int err) noexcept = 0;
    // Receiving failed, typically with an ICMP error queued on a connected socket. Receiving
    // stops; start_receiving_datagrams resumes it.
    virtual void on_error(int err) noexcept = 0;
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
    // Sends FIN once the send queue has drained, keeping the read side open. Closing with
    // unread input makes the kernel send RST, which can destroy a response the peer has not
    // read yet; an error response is followed by this, a short drain, then begin_close.
    virtual void shutdown_write(ConnId conn) noexcept = 0;
    // Drops unsent bytes, cancels in-flight work and closes the socket once the kernel holds
    // no reference to it. No callback for this connection follows.
    virtual void begin_close(ConnId conn) noexcept = 0;
    [[nodiscard]] virtual bool is_quiescent(ConnId conn) const noexcept = 0;

    // `socket` must be an IPv4 or IPv6 UDP socket. It starts out not receiving.
    [[nodiscard]] virtual std::expected<DatagramId, int>
    attach_datagram(os::UniqueFd socket, IDatagramHandler& handler) = 0;
    virtual void start_receiving_datagrams(DatagramId socket) noexcept = 0;
    // Exact, as stop_receiving is. Datagrams wait in the socket's receive buffer, and once that
    // is full the kernel drops new ones, which is the backpressure UDP has.
    virtual void stop_receiving_datagrams(DatagramId socket) noexcept = 0;
    // Copies `payload` and never blocks or queues without bound. EAGAIN means the datagram was
    // not taken and is the caller's to drop or retry: a late media packet is worth less than a
    // fresh one. The reactors refuse on different grounds: io_uring once it holds 1,024 sends
    // not yet completed, across all its sockets, which only happens when a single iteration
    // sends that many; epoll when the socket's kernel send buffer is full. Also
    // EMSGSIZE past kMaxDatagramSize, EAFNOSUPPORT for an IPv6 destination on an IPv4 socket,
    // and EBADF for a closed socket. Whatever the kernel refuses later arrives through
    // on_send_error, never from inside this call.
    [[nodiscard]] virtual std::expected<void, int>
    send_to(DatagramId socket, SocketAddr to, std::span<const std::byte> payload) noexcept = 0;
    // Drops sends in flight; no callback for this socket follows.
    virtual void begin_close(DatagramId socket) noexcept = 0;
    [[nodiscard]] virtual bool is_quiescent(DatagramId socket) const noexcept = 0;
    [[nodiscard]] virtual DatagramStats datagram_stats(DatagramId socket) const noexcept = 0;

    // The caller keeps ownership of `fd` and must unwatch() before closing it.
    [[nodiscard]] virtual std::expected<void, int> watch(int fd, Interest interest,
                                                         IReadyHandler& handler) = 0;
    virtual void unwatch(int fd) noexcept = 0;

    virtual TimerId arm_timer(core::Millis delay, ITimerHandler& handler) = 0;
    virtual void cancel_timer(TimerId timer) noexcept = 0;

    // Sampled once per iteration, before dispatch, and held for every callback in it. Between
    // iterations (before the first, or while the owner does other work) it is the clock's own
    // reading, so a deadline or timer set up there counts from when it was set.
    [[nodiscard]] virtual core::MonoTime now() const noexcept = 0;
    // Waits at most `max_wait`, dispatches, fires due timers. Returns events dispatched.
    virtual int run_once(core::Millis max_wait) = 0;
};

} // namespace net
