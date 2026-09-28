#include "uring_reactor.hpp"

#include <sys/socket.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <memory>
#include <poll.h>

namespace net::detail {

namespace {

// 4096 SQEs: each iteration submits at most a few per active connection, and the gateway
// caps connections at 512 per shard.
constexpr unsigned kRingEntries = 4096;
constexpr std::uint16_t kBufGroup = 1;
// 256 x 64 KiB = 16 MiB of receive buffers shared by every connection on the ring. Pages are
// only touched once the kernel writes into them, so idle connections cost nothing here.
constexpr unsigned kBufCount = 256;
constexpr std::size_t kBufSize = std::size_t{64} * 1024;
// The shortest delay the wheel expresses. Descriptors come back as soon as connections close;
// the pause only has to keep a listener that cannot accept from spinning the loop.
constexpr core::Millis kAcceptRetry = TimingWheel::kTick;
// A cancelled socket operation reports back within microseconds of the cancel reaching the
// kernel. A second without the last completion means it will not, and exiting beats hanging
// in a destructor. The wait is sliced so the deadline is overshot by 10 ms at most.
constexpr core::Millis kCancelGrace{1'000};
constexpr std::chrono::nanoseconds kCancelSlice = core::Millis{10};

// user_data: descriptor in the top 24 bits (16 M, far past any RLIMIT_NOFILE the slot table is
// sized for), generation in the next 32, operation in the low 8.
constexpr unsigned kSlotShift = 40;
constexpr unsigned kGenShift = 8;

std::uint64_t make_token(int fd, std::uint32_t gen, std::uint8_t op) noexcept {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(fd)) << kSlotShift) |
           (static_cast<std::uint64_t>(gen) << kGenShift) | op;
}

bool is_transient_accept_error(int err) noexcept {
    switch (err) {
    case ECONNABORTED:
    case EPROTO:
    case ENOPROTOOPT:
    case EHOSTDOWN:
    case ENONET:
    case EHOSTUNREACH:
    case EOPNOTSUPP:
    case ENETUNREACH:
    case ENETDOWN:
    case EPERM:
    case EINTR:
        return true;
    default:
        return false;
    }
}

} // namespace

bool io_uring_disabled(std::string_view sysctl) noexcept {
    const char* const end = std::to_address(sysctl.end());
    int value = 0;
    const auto [ptr, ec] = std::from_chars(std::to_address(sysctl.begin()), end, value);
    return ec == std::errc{} && ptr == end && value == 2;
}

std::expected<std::unique_ptr<UringReactor>, int> UringReactor::create(core::ports::IClock& clock,
                                                                       std::size_t max_fds) {
    auto reactor = std::make_unique<UringReactor>(clock, max_fds);
    io_uring_params params{};
    params.flags =
        IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SUBMIT_ALL;
    int rc = io_uring_queue_init_params(kRingEntries, &reactor->ring_, &params);
    if (rc < 0) {
        return std::unexpected(-rc);
    }
    reactor->ring_ready_ = true;
    reactor->buf_ring_ = io_uring_setup_buf_ring(&reactor->ring_, kBufCount, kBufGroup, 0, &rc);
    if (reactor->buf_ring_ == nullptr) {
        return std::unexpected(-rc);
    }
    for (unsigned i = 0; i < kBufCount; ++i) {
        io_uring_buf_ring_add(reactor->buf_ring_, reactor->buf_mem_.get() + (i * kBufSize),
                              kBufSize, static_cast<unsigned short>(i),
                              io_uring_buf_ring_mask(kBufCount), static_cast<int>(i));
    }
    io_uring_buf_ring_advance(reactor->buf_ring_, kBufCount);
    return reactor;
}

UringReactor::UringReactor(core::ports::IClock& clock, std::size_t max_fds)
    : clock_(clock),
      // NOLINTNEXTLINE(*-avoid-c-arrays): for_overwrite leaves the pages untouched.
      buf_mem_(std::make_unique_for_overwrite<std::byte[]>(kBufCount * kBufSize)), slots_(max_fds),
      wheel_(clock.now()), now_(clock.now()) {
    accept_retry_.self = this;
}

UringReactor::~UringReactor() {
    if (ring_ready_) {
        cancel_everything();
    }
    for (Slot& s : slots_) {
        s.sendq.clear(pool_);
        s.parked.clear(pool_);
    }
    if (buf_ring_ != nullptr) {
        io_uring_free_buf_ring(&ring_, buf_ring_, kBufCount, kBufGroup);
    }
    if (ring_ready_) {
        io_uring_queue_exit(&ring_);
    }
}

void UringReactor::cancel_everything() noexcept {
    // io_uring_queue_exit tears the ring down asynchronously, and until it does, pending
    // requests keep their sockets alive: a listener would still hold its port after the
    // process has "stopped". Cancel and reap everything here so shutdown is synchronous.
    std::size_t outstanding = orphans_;
    for (const Slot& s : slots_) {
        outstanding += s.in_flight;
    }
    if (outstanding == 0) {
        return;
    }
    io_uring_sqe* sqe = next_sqe();
    io_uring_prep_cancel64(sqe, 0, IORING_ASYNC_CANCEL_ANY);
    io_uring_sqe_set_data64(sqe, 0);
    const auto deadline = clock_.now() + kCancelGrace;
    while (outstanding > 0 && clock_.now() < deadline) {
        __kernel_timespec ts{.tv_sec = 0, .tv_nsec = kCancelSlice.count()};
        io_uring_cqe* first = nullptr;
        static_cast<void>(io_uring_submit_and_wait_timeout(&ring_, &first, 1, &ts, nullptr));
        unsigned head = 0;
        unsigned seen = 0;
        io_uring_cqe* cqe = nullptr;
        io_uring_for_each_cqe(&ring_, head, cqe) {
            ++seen;
            if (io_uring_cqe_get_data64(cqe) != 0 && (cqe->flags & IORING_CQE_F_MORE) == 0) {
                --outstanding;
            }
        }
        io_uring_cq_advance(&ring_, seen);
    }
}

bool UringReactor::alive(int fd, std::uint32_t gen) const noexcept {
    const Slot& s = slots_[static_cast<std::size_t>(fd)];
    return s.kind != Kind::Free && s.gen == gen && !s.closing;
}

io_uring_sqe* UringReactor::next_sqe() noexcept {
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (sqe == nullptr) {
        static_cast<void>(io_uring_submit(&ring_));
        sqe = io_uring_get_sqe(&ring_);
    }
    if (sqe == nullptr) {
        // Submitting empties the queue, so this means the ring itself is broken.
        std::abort();
    }
    return sqe;
}

void UringReactor::prepare(io_uring_sqe* sqe, int fd, Slot& s, Op op) noexcept {
    io_uring_sqe_set_data64(sqe, make_token(fd, s.gen, static_cast<std::uint8_t>(op)));
    ++s.in_flight;
}

void UringReactor::arm_recv(int fd, Slot& s) noexcept {
    // Single-shot on purpose. A multishot receive drains the whole socket backlog into the
    // buffer ring in one batch (measured: 60 completions, 3.9 MB), all of which lands after
    // a stop_receiving issued on the first. One receive at a time keeps backpressure exact.
    io_uring_sqe* sqe = next_sqe();
    io_uring_prep_recv(sqe, fd, nullptr, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = kBufGroup;
    prepare(sqe, fd, s, Op::Recv);
    s.recv_armed = true;
}

void UringReactor::arm_send(int fd, Slot& s) noexcept {
    const auto head = s.sendq.front();
    io_uring_sqe* sqe = next_sqe();
    io_uring_prep_send(sqe, fd, head.data(), head.size(), MSG_NOSIGNAL);
    prepare(sqe, fd, s, Op::Send);
    s.send_armed = true;
}

void UringReactor::arm_poll(int fd, Slot& s) noexcept {
    unsigned mask = 0;
    if (has(s.interest, Interest::Read)) {
        mask |= POLLIN;
    }
    if (has(s.interest, Interest::Write)) {
        mask |= POLLOUT;
    }
    // Single-shot, re-armed after each report: re-arming on a still-ready descriptor fires at
    // once, which gives libcurl and libpq the level-triggered semantics they expect.
    io_uring_sqe* sqe = next_sqe();
    io_uring_prep_poll_add(sqe, fd, mask);
    prepare(sqe, fd, s, Op::Poll);
    s.poll_armed = true;
}

void UringReactor::arm_accept(int fd, Slot& s) noexcept {
    io_uring_sqe* sqe = next_sqe();
    io_uring_prep_multishot_accept(sqe, fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    prepare(sqe, fd, s, Op::Accept);
}

void UringReactor::cancel_op(int fd, Slot& s, Op op) noexcept {
    io_uring_sqe* sqe = next_sqe();
    io_uring_prep_cancel64(sqe, make_token(fd, s.gen, static_cast<std::uint8_t>(op)), 0);
    prepare(sqe, fd, s, Op::Cancel);
}

void UringReactor::cancel_all(int fd, Slot& s) noexcept {
    io_uring_sqe* sqe = next_sqe();
    io_uring_prep_cancel_fd(sqe, fd, IORING_ASYNC_CANCEL_ALL);
    prepare(sqe, fd, s, Op::Cancel);
}

// From here on the connection only waits for begin_close, like an epoll descriptor that has
// left the interest set: bytes sent later are dropped, so the peer never receives what was
// queued behind bytes that were lost.
void UringReactor::fail(int fd, Slot& s) noexcept {
    s.failed = true;
    s.receiving = false;
    if (s.recv_armed) {
        cancel_op(fd, s, Op::Recv);
    }
}

void UringReactor::finalize(Slot& s) noexcept {
    assert(s.in_flight == 0);
    s.sendq.clear(pool_);
    s.parked.clear(pool_);
    s.owned.reset();
    s.stream = nullptr;
    s.ready = nullptr;
    s.acceptor = nullptr;
    s.receiving = s.recv_armed = s.send_armed = s.poll_armed = false;
    s.closing = s.failed = s.eof = s.eof_delivered = s.delivery_queued = s.accept_paused = false;
    s.interest = Interest::None;
    s.kind = Kind::Free;
    ++s.gen;
}

void UringReactor::return_buffer(std::uint16_t bid) noexcept {
    io_uring_buf_ring_add(buf_ring_, buf_mem_.get() + (static_cast<std::size_t>(bid) * kBufSize),
                          kBufSize, bid, io_uring_buf_ring_mask(kBufCount), 0);
    io_uring_buf_ring_advance(buf_ring_, 1);
}

void UringReactor::queue_delivery(int fd, Slot& s) noexcept {
    if (!s.delivery_queued) {
        s.delivery_queued = true;
        deliveries_.push_back(ConnId{.fd = fd, .gen = s.gen});
    }
}

std::expected<void, int> UringReactor::listen(os::UniqueFd listener, IAcceptHandler& handler) {
    const int fd = listener.get();
    if (fd < 0 || static_cast<std::size_t>(fd) >= slots_.size()) {
        return std::unexpected(EMFILE);
    }
    Slot& s = slots_[static_cast<std::size_t>(fd)];
    s.kind = Kind::Listener;
    s.owned = std::move(listener);
    s.acceptor = &handler;
    listeners_.push_back(fd);
    arm_accept(fd, s);
    return {};
}

void UringReactor::stop_listening() noexcept {
    for (const int fd : listeners_) {
        Slot& s = slots_[static_cast<std::size_t>(fd)];
        s.closing = true;
        if (s.in_flight == 0) {
            finalize(s);
        } else {
            cancel_all(fd, s);
        }
    }
    listeners_.clear();
    cancel_timer(accept_retry_.timer);
}

std::expected<ConnId, int> UringReactor::attach(os::UniqueFd conn, IStreamHandler& handler) {
    const int fd = conn.get();
    if (fd < 0 || static_cast<std::size_t>(fd) >= slots_.size()) {
        return std::unexpected(EMFILE);
    }
    Slot& s = slots_[static_cast<std::size_t>(fd)];
    if (s.kind != Kind::Free) {
        return std::unexpected(EEXIST);
    }
    s.kind = Kind::Stream;
    s.owned = std::move(conn);
    s.stream = &handler;
    return ConnId{.fd = fd, .gen = s.gen};
}

void UringReactor::start_receiving(ConnId conn) noexcept {
    Slot* s = stream_slot(conn);
    if (s == nullptr || s->receiving || s->failed || s->eof_delivered) {
        return;
    }
    s->receiving = true;
    if (!s->parked.empty() || s->eof) {
        // Delivered from the loop, never from inside this call: the caller is usually in
        // the middle of a callback and must not be re-entered.
        queue_delivery(conn.fd, *s);
    } else if (!s->recv_armed) {
        arm_recv(conn.fd, *s);
    }
}

void UringReactor::stop_receiving(ConnId conn) noexcept {
    Slot* s = stream_slot(conn);
    if (s == nullptr || !s->receiving) {
        return;
    }
    s->receiving = false;
    if (s->recv_armed) {
        cancel_op(conn.fd, *s, Op::Recv);
    }
}

void UringReactor::send(ConnId conn, std::span<const std::byte> bytes) noexcept {
    Slot* s = stream_slot(conn);
    if (s == nullptr || s->failed || bytes.empty()) {
        return;
    }
    if (s->sendq.size() + bytes.size() > kMaxSendQueue) {
        fail(conn.fd, *s);
        deferred_errors_.push_back({.conn = conn, .err = ENOBUFS});
        return;
    }
    s->sendq.append(bytes, pool_);
    if (!s->send_armed) {
        arm_send(conn.fd, *s);
    }
}

std::size_t UringReactor::pending_send_bytes(ConnId conn) const noexcept {
    const Slot* s = stream_slot(conn);
    return s == nullptr ? 0 : s->sendq.size();
}

void UringReactor::begin_close(ConnId conn) noexcept {
    Slot* s = stream_slot(conn);
    if (s == nullptr) {
        return;
    }
    s->closing = true;
    s->receiving = false;
    s->stream = nullptr;
    if (s->in_flight == 0) {
        finalize(*s);
    } else {
        // The kernel may still read the send queue or write a receive buffer; the slot and
        // its descriptor live until every operation has reported back.
        cancel_all(conn.fd, *s);
    }
}

bool UringReactor::is_quiescent(ConnId conn) const noexcept {
    if (conn.fd < 0 || static_cast<std::size_t>(conn.fd) >= slots_.size()) {
        return true;
    }
    const Slot& s = slots_[static_cast<std::size_t>(conn.fd)];
    return s.kind != Kind::Stream || s.gen != conn.gen;
}

std::expected<void, int> UringReactor::watch(int fd, Interest interest, IReadyHandler& handler) {
    if (fd < 0 || static_cast<std::size_t>(fd) >= slots_.size()) {
        return std::unexpected(EMFILE);
    }
    Slot& s = slots_[static_cast<std::size_t>(fd)];
    if (s.kind != Kind::Free && s.kind != Kind::Watch) {
        return std::unexpected(EEXIST);
    }
    const bool changed = s.kind == Kind::Free || s.interest != interest;
    s.kind = Kind::Watch;
    s.ready = &handler;
    s.interest = interest;
    if (!changed) {
        return {};
    }
    if (s.poll_armed) {
        // The cancelled poll's completion re-arms with the new mask.
        cancel_op(fd, s, Op::Poll);
    } else if (interest != Interest::None) {
        arm_poll(fd, s);
    }
    return {};
}

void UringReactor::unwatch(int fd) noexcept {
    if (fd < 0 || static_cast<std::size_t>(fd) >= slots_.size()) {
        return;
    }
    Slot& s = slots_[static_cast<std::size_t>(fd)];
    if (s.kind != Kind::Watch) {
        return;
    }
    if (s.poll_armed) {
        cancel_op(fd, s, Op::Poll);
    }
    // The library may close and reuse the number as soon as this returns, so the slot is
    // released now and its outstanding completions are recognised by their stale generation.
    orphans_ += s.in_flight;
    s.in_flight = 0;
    finalize(s);
}

TimerId UringReactor::arm_timer(core::Millis delay, ITimerHandler& handler) {
    return wheel_.arm(now_, delay, handler);
}

void UringReactor::cancel_timer(TimerId timer) noexcept {
    wheel_.cancel(timer);
}

int UringReactor::run_once(core::Millis max_wait) {
    core::Millis wait =
        deliveries_.empty() && deferred_errors_.empty() ? max_wait : core::Millis{0};
    if (const auto next = wheel_.next_expiry(now_)) {
        wait = std::min(wait, *next);
    }
    if (wait <= core::Millis{0}) {
        static_cast<void>(io_uring_submit_and_get_events(&ring_));
    } else {
        const auto secs = std::chrono::floor<std::chrono::seconds>(wait);
        __kernel_timespec ts{.tv_sec = secs.count(),
                             .tv_nsec = std::chrono::nanoseconds(wait - secs).count()};
        io_uring_cqe* first = nullptr;
        static_cast<void>(io_uring_submit_and_wait_timeout(&ring_, &first, 1, &ts, nullptr));
    }
    now_ = clock_.now();

    unsigned head = 0;
    unsigned seen = 0;
    io_uring_cqe* cqe = nullptr;
    io_uring_for_each_cqe(&ring_, head, cqe) {
        ++seen;
        dispatch(*cqe);
    }
    io_uring_cq_advance(&ring_, seen);
    run_deferred();
    return static_cast<int>(seen) + static_cast<int>(wheel_.tick_to(now_));
}

void UringReactor::dispatch(const io_uring_cqe& cqe) noexcept {
    const std::uint64_t token = io_uring_cqe_get_data64(&cqe);
    const auto fd = static_cast<int>(token >> kSlotShift);
    const auto gen = static_cast<std::uint32_t>((token >> kGenShift) & 0xFFFFFFFFU);
    const auto op = static_cast<Op>(token & 0xFFU);
    const bool more = (cqe.flags & IORING_CQE_F_MORE) != 0;
    std::span<const std::byte> data;
    std::optional<std::uint16_t> bid;
    if ((cqe.flags & IORING_CQE_F_BUFFER) != 0) {
        bid = static_cast<std::uint16_t>(cqe.flags >> IORING_CQE_BUFFER_SHIFT);
        if (cqe.res > 0) {
            data = {buf_mem_.get() + (static_cast<std::size_t>(*bid) * kBufSize),
                    static_cast<std::size_t>(cqe.res)};
        }
    }

    Slot& s = slots_[static_cast<std::size_t>(fd)];
    if (s.gen != gen || s.kind == Kind::Free) {
        if (bid) {
            return_buffer(*bid);
        }
        if (!more) {
            assert(orphans_ > 0);
            --orphans_;
        }
        return;
    }
    if (!more) {
        assert(s.in_flight > 0);
        --s.in_flight;
    }

    switch (op) {
    case Op::Recv:
        on_recv(fd, s, cqe.res, data);
        if (bid) {
            return_buffer(*bid);
        }
        break;
    case Op::Send:
        on_send(fd, s, cqe.res);
        break;
    case Op::Poll:
        on_poll(fd, s, cqe.res);
        break;
    case Op::Accept:
        on_accept(fd, s, cqe.res, more);
        break;
    case Op::Cancel:
        break;
    }

    if (s.gen == gen && s.closing && s.in_flight == 0) {
        finalize(s);
    }
}

void UringReactor::on_recv(int fd, Slot& s, int res, std::span<const std::byte> data) noexcept {
    s.recv_armed = false;
    if (s.closing || s.failed) {
        return;
    }
    const std::uint32_t gen = s.gen;
    if (res > 0) {
        if (s.receiving) {
            s.stream->on_data(data);
            if (!alive(fd, gen)) {
                return;
            }
        } else {
            s.parked.append(data, pool_);
        }
    } else if (res == 0) {
        s.eof = true;
        if (s.receiving) {
            s.receiving = false;
            s.eof_delivered = true;
            s.stream->on_peer_eof();
        }
        return;
    } else if (res != -ENOBUFS && res != -ECANCELED) {
        // ENOBUFS means the ring ran dry this round; every buffer is back before the re-arm
        // below reaches the kernel, so it simply tries again.
        fail(fd, s);
        s.stream->on_error(-res);
        return;
    }
    if (s.receiving && !s.recv_armed) {
        arm_recv(fd, s);
    }
}

void UringReactor::on_send(int fd, Slot& s, int res) noexcept {
    s.send_armed = false;
    if (s.closing || s.failed) {
        return;
    }
    if (res < 0) {
        if (res != -ECANCELED) {
            fail(fd, s);
            s.stream->on_error(-res);
        }
        return;
    }
    s.sendq.consume(static_cast<std::size_t>(res), pool_);
    if (!s.sendq.empty()) {
        arm_send(fd, s);
        return;
    }
    s.stream->on_writable();
}

void UringReactor::on_poll(int fd, Slot& s, int res) noexcept {
    s.poll_armed = false;
    if (s.kind != Kind::Watch) {
        return;
    }
    const std::uint32_t gen = s.gen;
    if (res != -ECANCELED) {
        Interest ready = Interest::None;
        const auto revents = res < 0 ? static_cast<unsigned>(POLLERR) : static_cast<unsigned>(res);
        if ((revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            ready = ready | Interest::Read;
        }
        if ((revents & (POLLOUT | POLLERR)) != 0) {
            ready = ready | Interest::Write;
        }
        s.ready->on_ready(ready);
        if (!alive(fd, gen)) {
            return;
        }
    }
    if (!s.poll_armed && s.interest != Interest::None) {
        arm_poll(fd, s);
    }
}

void UringReactor::on_accept(int fd, Slot& s, int res, bool more) noexcept {
    if (res >= 0) {
        os::UniqueFd conn{res};
        if (!s.closing) {
            s.acceptor->on_accept(std::move(conn));
        }
    } else if (!is_transient_accept_error(-res) && res != -ECANCELED && !s.closing) {
        // EMFILE/ENFILE/ENOBUFS: the connection stays in the backlog and retrying at once
        // would fail the same way. Look again once some descriptors have been released.
        if (more) {
            cancel_op(fd, s, Op::Accept);
        }
        s.accept_paused = true;
        wheel_.cancel(accept_retry_.timer);
        accept_retry_.timer = wheel_.arm(now_, kAcceptRetry, accept_retry_);
        return;
    }
    if (!more && !s.closing && !s.accept_paused && s.kind == Kind::Listener) {
        arm_accept(fd, s);
    }
}

void UringReactor::AcceptRetry::on_timeout() noexcept {
    for (const int fd : self->listeners_) {
        Slot& s = self->slots_[static_cast<std::size_t>(fd)];
        if (s.accept_paused && !s.closing) {
            s.accept_paused = false;
            self->arm_accept(fd, s);
        }
    }
}

void UringReactor::run_deferred() noexcept {
    // Handlers queue more while this runs; they land in the swapped-in queue, for next time.
    delivering_.swap(deliveries_);
    for (const ConnId conn : delivering_) {
        Slot* s = stream_slot(conn);
        if (s == nullptr) {
            continue;
        }
        s->delivery_queued = false;
        while (s->receiving && !s->parked.empty()) {
            const auto chunk = s->parked.front();
            s->stream->on_data(chunk);
            if (!alive(conn.fd, conn.gen)) {
                break;
            }
            s->parked.consume(chunk.size(), pool_);
        }
        if (!alive(conn.fd, conn.gen) || !s->receiving || !s->parked.empty()) {
            continue;
        }
        if (s->eof) {
            s->receiving = false;
            s->eof_delivered = true;
            s->stream->on_peer_eof();
        } else if (!s->recv_armed) {
            arm_recv(conn.fd, *s);
        }
    }
    delivering_.clear();

    reporting_.swap(deferred_errors_);
    for (const DeferredError& e : reporting_) {
        if (Slot* s = stream_slot(e.conn)) {
            s->stream->on_error(e.err);
        }
    }
    reporting_.clear();
}

} // namespace net::detail
