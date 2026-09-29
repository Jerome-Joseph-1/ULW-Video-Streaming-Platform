#pragma once

#include "core/ports/clock.hpp"
#include "net/reactor.hpp"

#include "send_queue.hpp"
#include "timing_wheel.hpp"

#include <array>
#include <liburing.h>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace net::detail {

// `sysctl` is the value of kernel.io_uring_disabled: 0 enabled, 1 restricted to a group,
// 2 disabled. io_uring_setup itself refuses the restricted case, so only an outright 2 counts.
[[nodiscard]] bool io_uring_disabled(std::string_view sysctl) noexcept;

// Must be created on the thread that will run it: SINGLE_ISSUER binds the ring to its
// creator, and DEFER_TASKRUN only posts completions when that thread enters the kernel.
class UringReactor final : public IReactor {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<UringReactor>, int>
    create(core::ports::IClock& clock, std::size_t max_fds);

    UringReactor(core::ports::IClock& clock, std::size_t max_fds);
    ~UringReactor() override;
    UringReactor(const UringReactor&) = delete;
    UringReactor& operator=(const UringReactor&) = delete;

    [[nodiscard]] std::expected<void, int> listen(os::UniqueFd listener,
                                                  IAcceptHandler& handler) override;
    void stop_listening() noexcept override;
    [[nodiscard]] std::expected<ConnId, int> attach(os::UniqueFd conn,
                                                    IStreamHandler& handler) override;
    void start_receiving(ConnId conn) noexcept override;
    void stop_receiving(ConnId conn) noexcept override;
    void send(ConnId conn, std::span<const std::byte> bytes) noexcept override;
    [[nodiscard]] std::size_t pending_send_bytes(ConnId conn) const noexcept override;
    void shutdown_write(ConnId conn) noexcept override;
    void begin_close(ConnId conn) noexcept override;
    [[nodiscard]] bool is_quiescent(ConnId conn) const noexcept override;
    [[nodiscard]] std::expected<DatagramId, int>
    attach_datagram(os::UniqueFd socket, IDatagramHandler& handler) override;
    void start_receiving_datagrams(DatagramId socket) noexcept override;
    void stop_receiving_datagrams(DatagramId socket) noexcept override;
    [[nodiscard]] std::expected<void, int>
    send_to(DatagramId socket, SocketAddr to, std::span<const std::byte> payload) noexcept override;
    void begin_close(DatagramId socket) noexcept override;
    [[nodiscard]] bool is_quiescent(DatagramId socket) const noexcept override;
    [[nodiscard]] DatagramStats datagram_stats(DatagramId socket) const noexcept override;
    [[nodiscard]] std::expected<void, int> watch(int fd, Interest interest,
                                                 IReadyHandler& handler) override;
    void unwatch(int fd) noexcept override;
    TimerId arm_timer(core::Millis delay, ITimerHandler& handler) override;
    void cancel_timer(TimerId timer) noexcept override;
    [[nodiscard]] core::MonoTime now() const noexcept override { return now_; }
    int run_once(core::Millis max_wait) override;

private:
    enum class Kind : std::uint8_t { Free, Stream, Watch, Listener, Datagram };
    enum class Op : std::uint8_t {
        Recv = 1,
        Send,
        Cancel,
        Poll,
        Accept,
        DatagramRecv,
        SendTo,
        // The zero-copy probe's send, whose completions may outlive the probe.
        Probe
    };

    struct Slot {
        Kind kind = Kind::Free;
        std::uint32_t gen = 1;
        std::uint16_t in_flight = 0;
        bool receiving = false;
        bool recv_armed = false;
        bool send_armed = false;
        bool poll_armed = false;
        bool closing = false;
        bool failed = false;
        bool eof = false;
        bool shut_pending = false;
        bool eof_delivered = false;
        bool delivery_queued = false;
        bool accept_paused = false;
        bool v6 = false;
        // Sends go out as SENDMSG_ZC until the kernel reports it copied one anyway.
        bool zero_copy = false;
        // A datagram receive ended with every buffer in use; it is re-armed once one is back.
        bool starved = false;
        Interest interest = Interest::None;
        // Datagrams that arrived after stop_receiving_datagrams, still in their ring buffers:
        // a list threaded through held_next_, oldest first.
        std::int32_t held_head = -1;
        std::int32_t held_tail = -1;
        std::uint16_t held_count = 0;
        // A receive error that arrived after a stop. The failed receive consumed the socket's
        // error, so it is reported here, after the held datagrams, once receiving resumes.
        int held_error = 0;
        os::UniqueFd owned;
        IStreamHandler* stream = nullptr;
        IReadyHandler* ready = nullptr;
        IAcceptHandler* acceptor = nullptr;
        IDatagramHandler* dgram = nullptr;
        ByteQueue sendq;
        // Bytes a receive in flight delivered after stop_receiving. One single-shot receive
        // is outstanding at most, so this never exceeds one buffer.
        ByteQueue parked;
        DatagramStats stats;
    };

    // Everything a SENDMSG reads, kept still until its completion. Not zeroed: the payload is
    // only read after being written.
    struct PendingSend { // NOLINT(cppcoreguidelines-pro-type-member-init)
        int fd = -1;
        SocketAddr to;
        msghdr msg{};
        iovec iov{};
        sockaddr_storage addr{};
        std::array<std::byte, kMaxDatagramSize> payload;
    };

    struct Datagram {
        SocketAddr from;
        std::span<const std::byte> payload;
        bool truncated = false;
    };

    struct AcceptRetry final : ITimerHandler {
        UringReactor* self = nullptr;
        TimerId timer;
        void on_timeout() noexcept override;
    };

    struct DeferredError {
        ConnId conn;
        int err = 0;
    };

    [[nodiscard]] auto* stream_slot(this auto& self, ConnId conn) noexcept {
        using SlotPtr = decltype(&self.slots_[0]);
        if (conn.fd < 0 || static_cast<std::size_t>(conn.fd) >= self.slots_.size()) {
            return SlotPtr{nullptr};
        }
        auto& s = self.slots_[static_cast<std::size_t>(conn.fd)];
        return s.kind == Kind::Stream && s.gen == conn.gen && !s.closing ? &s : SlotPtr{nullptr};
    }
    [[nodiscard]] auto* datagram_slot(this auto& self, DatagramId socket) noexcept {
        using SlotPtr = decltype(&self.slots_[0]);
        if (socket.fd < 0 || static_cast<std::size_t>(socket.fd) >= self.slots_.size()) {
            return SlotPtr{nullptr};
        }
        auto& s = self.slots_[static_cast<std::size_t>(socket.fd)];
        return s.kind == Kind::Datagram && s.gen == socket.gen && !s.closing ? &s
                                                                             : SlotPtr{nullptr};
    }
    [[nodiscard]] bool alive(int fd, std::uint32_t gen) const noexcept;

    [[nodiscard]] io_uring_sqe* next_sqe() noexcept;
    [[nodiscard]] bool probe_zero_copy_send() noexcept;
    static void prepare(io_uring_sqe* sqe, int fd, Slot& s, Op op) noexcept;
    void arm_recv(int fd, Slot& s) noexcept;
    void arm_send(int fd, Slot& s) noexcept;
    void arm_poll(int fd, Slot& s) noexcept;
    void arm_accept(int fd, Slot& s) noexcept;
    void cancel_op(int fd, Slot& s, Op op) noexcept;
    void cancel_all(int fd, Slot& s) noexcept;
    void fail(int fd, Slot& s) noexcept;
    void finalize(Slot& s) noexcept;
    void return_buffer(std::uint16_t bid) noexcept;
    void arm_datagram_recv(int fd, Slot& s) noexcept;
    [[nodiscard]] Datagram parse_datagram(std::uint16_t bid, int res) noexcept;
    void return_datagram_buffer(std::uint16_t bid) noexcept;
    void hold(Slot& s, std::uint16_t bid, int res) noexcept;
    [[nodiscard]] std::uint16_t pop_held(Slot& s) noexcept;
    void release_held(Slot& s) noexcept;
    [[nodiscard]] bool datagram_buffer_free() const noexcept;
    void queue_delivery(int fd, Slot& s) noexcept;

    void dispatch(const io_uring_cqe& cqe) noexcept;
    void on_recv(int fd, Slot& s, int res, std::span<const std::byte> data) noexcept;
    void on_send(int fd, Slot& s, int res) noexcept;
    void on_poll(int fd, Slot& s, int res) noexcept;
    void on_accept(int fd, Slot& s, int res, bool more) noexcept;
    void on_datagram_recv(int fd, Slot& s, int res, std::optional<std::uint16_t> bid,
                          bool more) noexcept;
    void take_datagram(Slot& s, std::uint16_t bid, int res) noexcept;
    void on_send_to(std::size_t index, const io_uring_cqe& cqe) noexcept;
    void deliver_held_datagrams() noexcept;
    void rearm_starved() noexcept;
    void run_deferred() noexcept;
    void cancel_everything() noexcept;

    core::ports::IClock& clock_;
    io_uring ring_{};
    bool ring_ready_ = false;
    io_uring_buf_ring* buf_ring_ = nullptr;
    std::unique_ptr<std::byte[]> buf_mem_; // NOLINT(*-avoid-c-arrays): see the constructor
    std::vector<Slot> slots_;
    // Datagrams get their own group of small buffers: a 2 KiB datagram parked in a 64 KiB
    // stream buffer would waste 62 KiB, and a burst of datagrams must not starve the streams.
    io_uring_buf_ring* dgram_ring_ = nullptr;
    std::unique_ptr<std::byte[]> dgram_mem_; // NOLINT(*-avoid-c-arrays): see the constructor
    std::vector<std::int32_t> held_next_;
    std::vector<std::int32_t> held_len_;
    std::size_t held_count_ = 0;
    // Multishot RECVMSG reads the name and control lengths from here when it is armed.
    msghdr recv_msg_{};
    std::vector<std::unique_ptr<PendingSend>> send_pool_;
    std::vector<std::uint32_t> free_sends_;
    std::size_t sends_in_flight_ = 0;
    bool zero_copy_supported_ = false;
    std::vector<DatagramId> datagram_deliveries_;
    std::vector<DatagramId> delivering_datagrams_;
    std::vector<DatagramId> starved_;
    TimingWheel wheel_;
    ChunkPool pool_;
    core::MonoTime now_;
    // Operations still in flight for slots that were released while the kernel held them
    // (watched descriptors are handed back to their library on unwatch).
    std::size_t orphans_ = 0;
    std::vector<int> listeners_;
    std::vector<ConnId> deliveries_;
    std::vector<DeferredError> deferred_errors_;
    // run_deferred trades these for the queues above and hands them back empty, so neither
    // pair gives up its capacity and a warm loop queues without allocating.
    std::vector<ConnId> delivering_;
    std::vector<DeferredError> reporting_;
    AcceptRetry accept_retry_;
};

} // namespace net::detail
