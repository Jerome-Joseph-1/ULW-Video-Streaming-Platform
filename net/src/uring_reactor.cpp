#include "uring_reactor.hpp"

#include "net/socket.hpp"

#include "sockaddr.hpp"

#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <poll.h>
#include <utility>

namespace net::detail {

namespace {

// 4096 SQEs: each iteration submits at most a few per active connection, and the gateway
// caps connections at 448 per shard.
constexpr unsigned kRingEntries = 4096;
constexpr std::uint16_t kBufGroup = 1;
// 256 x 64 KiB = 16 MiB of receive buffers shared by every connection on the ring. Pages are
// only touched once the kernel writes into them, so idle connections cost nothing here.
constexpr unsigned kBufCount = 256;
constexpr std::size_t kBufSize = std::size_t{64} * 1024;
constexpr std::uint16_t kDatagramGroup = 2;
// A default 208 KiB receive buffer holds 256 small datagrams (832 bytes of truesize each,
// measured on 6.18). 512 buffers let two sockets drain a full receive buffer in the same batch
// before the ring runs dry.
constexpr unsigned kDatagramBufCount = 512;
// A stopped socket holds at most an eighth of the ring, so eight have to stop mid-burst at once
// before a socket still receiving finds it empty. 64 is also what one epoll wakeup reads.
constexpr std::uint16_t kMaxHeldPerSocket = 64;
// RECVMSG writes a header, then the source address in the room msg_namelen reserves, then the
// payload: 16 + 28 + 2048 = 2092 bytes, 1 MiB for the ring.
constexpr std::size_t kDatagramBufSize =
    sizeof(io_uring_recvmsg_out) + sizeof(sockaddr_in6) + kMaxDatagramSize;
// At 1 Gbit/s of 1200-byte datagrams (about 104,000 a second) and a loop turning every
// millisecond, about 104 sends are submitted per iteration and complete in the next, so 1024 is
// five times the steady state. The limit is per reactor, not per socket: one socket may carry
// the whole rate, as a socket facing the SFU does. A pending send is about
// 2.3 KB (the payload, a sockaddr_storage and a msghdr), so 1024 of them take about 2.3 MiB.
constexpr std::size_t kMaxSendsInFlight = 1024;
// A send's pool index replaces the descriptor and generation in its user_data: the pending
// send records the descriptor, and its socket cannot be recycled while the send is in flight.
constexpr unsigned kSendIndexShift = 8;
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

[[nodiscard]] bool locked_memory_is_unlimited() noexcept {
    rlimit limit{};
    return ::getrlimit(RLIMIT_MEMLOCK, &limit) == 0 && limit.rlim_cur == RLIM_INFINITY;
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
    reactor->dgram_ring_ =
        io_uring_setup_buf_ring(&reactor->ring_, kDatagramBufCount, kDatagramGroup, 0, &rc);
    if (reactor->dgram_ring_ == nullptr) {
        return std::unexpected(-rc);
    }
    for (unsigned i = 0; i < kDatagramBufCount; ++i) {
        io_uring_buf_ring_add(reactor->dgram_ring_,
                              reactor->dgram_mem_.get() + (i * kDatagramBufSize), kDatagramBufSize,
                              static_cast<unsigned short>(i),
                              io_uring_buf_ring_mask(kDatagramBufCount), static_cast<int>(i));
    }
    io_uring_buf_ring_advance(reactor->dgram_ring_, kDatagramBufCount);
    reactor->zero_copy_supported_ = reactor->probe_zero_copy_send();
    return reactor;
}

UringReactor::UringReactor(core::ports::IClock& clock, std::size_t max_fds)
    : clock_(clock),
      // NOLINTNEXTLINE(*-avoid-c-arrays): for_overwrite leaves the pages untouched.
      buf_mem_(std::make_unique_for_overwrite<std::byte[]>(kBufCount * kBufSize)), slots_(max_fds),
      // NOLINTNEXTLINE(*-avoid-c-arrays): for_overwrite leaves the pages untouched.
      dgram_mem_(std::make_unique_for_overwrite<std::byte[]>(kDatagramBufCount * kDatagramBufSize)),
      held_next_(kDatagramBufCount, -1), held_len_(kDatagramBufCount, 0), wheel_(clock.now()),
      now_(clock.now()) {
    accept_retry_.self = this;
    recv_msg_.msg_namelen = sizeof(sockaddr_in6);
    send_pool_.reserve(kMaxSendsInFlight);
    free_sends_.reserve(kMaxSendsInFlight);
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
    if (dgram_ring_ != nullptr) {
        io_uring_free_buf_ring(&ring_, dgram_ring_, kDatagramBufCount, kDatagramGroup);
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
            const std::uint64_t token = io_uring_cqe_get_data64(cqe);
            if (token != 0 && token != static_cast<std::uint8_t>(Op::Probe) &&
                (cqe->flags & IORING_CQE_F_MORE) == 0) {
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

// Zero copy is only used where the kernel also says whether it managed it
// (IORING_SEND_ZC_REPORT_USAGE, 6.2): 6.1 has SENDMSG_ZC but rejects the flag with EINVAL, and
// the opcode probe cannot tell the two apart, so one real send to ourselves decides.
//
// Every zero-copy send in flight charges two pages of a datagram to the locked-memory counter its
// user shares across all its processes, checked against RLIMIT_MEMLOCK unless the process has
// CAP_IPC_LOCK. kMaxSendsInFlight of them are 8 MiB, the whole of the usual default limit, and
// since 6.14 every ring's own pages are charged to the same counter. Under any finite limit, the
// burst below (or a busy socket) could refuse another reactor of the same user its ring with
// ENOMEM, so zero copy is only tried where the limit is unlimited.
bool UringReactor::probe_zero_copy_send() noexcept {
    if (!locked_memory_is_unlimited()) {
        return false;
    }
    io_uring_probe* probe = io_uring_get_probe_ring(&ring_);
    if (probe == nullptr) {
        return false;
    }
    const bool known = io_uring_opcode_supported(probe, IORING_OP_SENDMSG_ZC) != 0;
    io_uring_free_probe(probe);
    if (!known) {
        return false;
    }
    auto fd = bind_udp(SocketAddr::loopback(AddrFamily::V4, 0));
    if (!fd) {
        return false;
    }
    const auto self = local_addr(fd->get());
    if (!self || !to_sockaddr(*self, false, probe_.addr)) {
        return false;
    }
    probe_.iov = {.iov_base = &probe_.payload, .iov_len = 1};
    probe_.msg.msg_name = &probe_.addr;
    probe_.msg.msg_namelen = sizeof(sockaddr_in);
    probe_.msg.msg_iov = &probe_.iov;
    probe_.msg.msg_iovlen = 1;
    // As many sends at once as the reactor ever holds, so that a kernel which refuses some of a
    // burst (where one send alone succeeds) is never trusted with zero copy.
    for (std::size_t i = 0; i < kMaxSendsInFlight; ++i) {
        io_uring_sqe* sqe = next_sqe();
        io_uring_prep_sendmsg_zc(sqe, fd->get(), &probe_.msg, 0);
        sqe->ioprio |= IORING_SEND_ZC_REPORT_USAGE;
        io_uring_sqe_set_data64(sqe, static_cast<std::uint8_t>(Op::Probe));
    }
    // Every send completes inline on a local socket and its notification follows at once.
    // Waiting is bounded by count, not by the clock, which a test may hold still; if it runs
    // out, the late completions are recognised by their operation and ignored.
    std::size_t succeeded = 0;
    std::size_t finished = 0;
    const auto waits = kCancelGrace / kCancelSlice;
    for (auto i = decltype(waits){0}; finished < kMaxSendsInFlight && i < waits; ++i) {
        __kernel_timespec ts{.tv_sec = 0, .tv_nsec = kCancelSlice.count()};
        io_uring_cqe* first = nullptr;
        static_cast<void>(io_uring_submit_and_wait_timeout(&ring_, &first, 1, &ts, nullptr));
        unsigned head = 0;
        unsigned seen = 0;
        io_uring_cqe* cqe = nullptr;
        io_uring_for_each_cqe(&ring_, head, cqe) {
            ++seen;
            if ((cqe->flags & IORING_CQE_F_NOTIF) == 0 && cqe->res == 1) {
                ++succeeded;
            }
            if ((cqe->flags & IORING_CQE_F_MORE) == 0) {
                ++finished;
            }
        }
        io_uring_cq_advance(&ring_, seen);
    }
    return finished == kMaxSendsInFlight && succeeded == kMaxSendsInFlight;
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
    // The SQE is the kernel's ABI struct, unions included, and liburing 2.5 has no setter.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
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
    s.dgram = nullptr;
    s.stats = {};
    s.v6 = s.starved = s.zero_copy = false;
    s.held_error = 0;
    s.receiving = s.recv_armed = s.send_armed = s.poll_armed = false;
    s.closing = s.failed = s.eof = s.eof_delivered = s.delivery_queued = s.accept_paused = false;
    s.shut_pending = false;
    s.interest = Interest::None;
    s.kind = Kind::Free;
    ++s.gen;
}

void UringReactor::return_buffer(std::uint16_t bid) noexcept {
    io_uring_buf_ring_add(buf_ring_, buf_mem_.get() + (static_cast<std::size_t>(bid) * kBufSize),
                          kBufSize, bid, io_uring_buf_ring_mask(kBufCount), 0);
    io_uring_buf_ring_advance(buf_ring_, 1);
}

void UringReactor::arm_datagram_recv(int fd, Slot& s) noexcept {
    // Multishot, unlike stream receives (ADR-0021): each completion is one whole datagram, and
    // the few already posted when stop_receiving_datagrams runs are held in their buffers,
    // bounded by the ring, instead of the megabytes a stream socket can have queued.
    io_uring_sqe* sqe = next_sqe();
    io_uring_prep_recvmsg_multishot(sqe, fd, &recv_msg_, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): see arm_recv.
    sqe->buf_group = kDatagramGroup;
    prepare(sqe, fd, s, Op::DatagramRecv);
    s.recv_armed = true;
}

UringReactor::Datagram UringReactor::parse_datagram(std::uint16_t bid, int res) noexcept {
    std::byte* const buf = dgram_mem_.get() + (static_cast<std::size_t>(bid) * kDatagramBufSize);
    io_uring_recvmsg_out* out = io_uring_recvmsg_validate(buf, res, &recv_msg_);
    if (out == nullptr) {
        // Shorter than the header the kernel always writes: dropped like a truncated datagram
        // rather than parsed.
        return {.from = {}, .payload = {}, .truncated = true};
    }
    const auto* name = static_cast<const std::byte*>(io_uring_recvmsg_name(out));
    const auto* payload = static_cast<const std::byte*>(io_uring_recvmsg_payload(out, &recv_msg_));
    return {.from =
                from_sockaddr({name, std::min<std::size_t>(out->namelen, recv_msg_.msg_namelen)}),
            .payload = {payload, io_uring_recvmsg_payload_length(out, res, &recv_msg_)},
            .truncated = (out->flags & MSG_TRUNC) != 0};
}

void UringReactor::return_datagram_buffer(std::uint16_t bid) noexcept {
    io_uring_buf_ring_add(dgram_ring_,
                          dgram_mem_.get() + (static_cast<std::size_t>(bid) * kDatagramBufSize),
                          kDatagramBufSize, bid, io_uring_buf_ring_mask(kDatagramBufCount), 0);
    io_uring_buf_ring_advance(dgram_ring_, 1);
}

void UringReactor::hold(Slot& s, std::uint16_t bid, int res) noexcept {
    if (!s.receiving && s.held_count >= kMaxHeldPerSocket) {
        // Dropped as it would have been had the socket's receive buffer been full, rather than
        // let one stopped socket pin the ring every other socket shares.
        ++s.stats.stopped_drops;
        return_datagram_buffer(bid);
        return;
    }
    ++s.held_count;
    held_next_[bid] = -1;
    held_len_[bid] = res;
    if (s.held_tail < 0) {
        s.held_head = bid;
    } else {
        held_next_[static_cast<std::size_t>(s.held_tail)] = bid;
    }
    s.held_tail = bid;
    ++held_count_;
}

std::uint16_t UringReactor::pop_held(Slot& s) noexcept {
    const auto bid = static_cast<std::uint16_t>(s.held_head);
    s.held_head = held_next_[bid];
    if (s.held_head < 0) {
        s.held_tail = -1;
    }
    --s.held_count;
    --held_count_;
    return bid;
}

void UringReactor::release_held(Slot& s) noexcept {
    while (s.held_head >= 0) {
        return_datagram_buffer(pop_held(s));
    }
}

bool UringReactor::datagram_buffer_free() const noexcept {
    return held_count_ < kDatagramBufCount;
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
    // A chunk that cannot be had fails this connection, not the process.
    try {
        s->sendq.append(bytes, pool_);
    } catch (const std::bad_alloc&) {
        fail(conn.fd, *s);
        deferred_errors_.push_back({.conn = conn, .err = ENOMEM});
        return;
    }
    if (!s->send_armed) {
        arm_send(conn.fd, *s);
    }
}

std::size_t UringReactor::pending_send_bytes(ConnId conn) const noexcept {
    const Slot* s = stream_slot(conn);
    return s == nullptr ? 0 : s->sendq.size();
}

void UringReactor::shutdown_write(ConnId conn) noexcept {
    Slot* s = stream_slot(conn);
    if (s == nullptr) {
        return;
    }
    if (s->sendq.empty() && !s->send_armed) {
        static_cast<void>(::shutdown(conn.fd, SHUT_WR));
    } else {
        s->shut_pending = true;
    }
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

std::expected<DatagramId, int> UringReactor::attach_datagram(os::UniqueFd socket,
                                                             IDatagramHandler& handler) {
    const int fd = socket.get();
    if (fd < 0 || static_cast<std::size_t>(fd) >= slots_.size()) {
        return std::unexpected(EMFILE);
    }
    Slot& s = slots_[static_cast<std::size_t>(fd)];
    if (s.kind != Kind::Free) {
        return std::unexpected(EEXIST);
    }
    const auto v6 = udp_socket_is_v6(fd);
    if (!v6) {
        return std::unexpected(v6.error());
    }
    s.kind = Kind::Datagram;
    s.owned = std::move(socket);
    s.dgram = &handler;
    s.v6 = *v6;
    s.zero_copy = zero_copy_supported_;
    return DatagramId{.fd = fd, .gen = s.gen};
}

void UringReactor::start_receiving_datagrams(DatagramId socket) noexcept {
    Slot* s = datagram_slot(socket);
    if (s == nullptr || s->receiving) {
        return;
    }
    s->receiving = true;
    if (s->held_head >= 0 || s->held_error != 0) {
        // Delivered from the loop, never from inside this call, as for streams.
        if (!s->delivery_queued) {
            s->delivery_queued = true;
            datagram_deliveries_.push_back(socket);
        }
    } else if (!s->recv_armed && !s->starved) {
        arm_datagram_recv(socket.fd, *s);
    }
}

void UringReactor::stop_receiving_datagrams(DatagramId socket) noexcept {
    Slot* s = datagram_slot(socket);
    if (s == nullptr || !s->receiving) {
        return;
    }
    s->receiving = false;
    if (s->recv_armed) {
        cancel_op(socket.fd, *s, Op::DatagramRecv);
    }
}

std::expected<void, int> UringReactor::send_to(DatagramId socket, SocketAddr to,
                                               std::span<const std::byte> payload) noexcept {
    Slot* s = datagram_slot(socket);
    if (s == nullptr) {
        return std::unexpected(EBADF);
    }
    if (payload.size() > kMaxDatagramSize) {
        return std::unexpected(EMSGSIZE);
    }
    sockaddr_storage addr{};
    const auto addr_len = to_sockaddr(to, s->v6, addr);
    if (!addr_len) {
        return std::unexpected(addr_len.error());
    }
    if (sends_in_flight_ >= kMaxSendsInFlight) {
        ++s->stats.send_refused;
        return std::unexpected(EAGAIN);
    }
    std::uint32_t index = 0;
    if (free_sends_.empty()) {
        // Grows to the working set once; the capacity was reserved up front.
        index = static_cast<std::uint32_t>(send_pool_.size());
        send_pool_.push_back(std::make_unique_for_overwrite<PendingSend>());
    } else {
        index = free_sends_.back();
        free_sends_.pop_back();
    }
    PendingSend& p = *send_pool_[index];
    p.fd = socket.fd;
    p.to = to;
    p.addr = addr;
    std::ranges::copy(payload, p.payload.begin());
    p.iov = {.iov_base = p.payload.data(), .iov_len = payload.size()};
    p.msg = {};
    p.msg.msg_name = &p.addr;
    p.msg.msg_namelen = *addr_len;
    p.msg.msg_iov = &p.iov;
    p.msg.msg_iovlen = 1;
    p.pending = 0;
    submit_send(index, *s, s->zero_copy);
    ++sends_in_flight_;
    return {};
}

void UringReactor::submit_send(std::uint32_t index, Slot& s, bool zero_copy) noexcept {
    PendingSend& p = *send_pool_[index];
    io_uring_sqe* sqe = next_sqe();
    if (zero_copy) {
        io_uring_prep_sendmsg_zc(sqe, p.fd, &p.msg, 0);
        sqe->ioprio |= IORING_SEND_ZC_REPORT_USAGE;
        ++s.stats.zero_copy_sends;
    } else {
        io_uring_prep_sendmsg(sqe, p.fd, &p.msg, 0);
    }
    io_uring_sqe_set_data64(sqe, (std::uint64_t{index} << kSendIndexShift) |
                                     static_cast<std::uint8_t>(Op::SendTo));
    p.zero_copy = zero_copy;
    ++p.pending;
    ++s.in_flight;
}

void UringReactor::begin_close(DatagramId socket) noexcept {
    Slot* s = datagram_slot(socket);
    if (s == nullptr) {
        return;
    }
    s->closing = true;
    s->receiving = false;
    s->dgram = nullptr;
    release_held(*s);
    if (s->in_flight == 0) {
        finalize(*s);
    } else {
        cancel_all(socket.fd, *s);
    }
}

bool UringReactor::is_quiescent(DatagramId socket) const noexcept {
    if (socket.fd < 0 || static_cast<std::size_t>(socket.fd) >= slots_.size()) {
        return true;
    }
    const Slot& s = slots_[static_cast<std::size_t>(socket.fd)];
    return s.kind != Kind::Datagram || s.gen != socket.gen;
}

DatagramStats UringReactor::datagram_stats(DatagramId socket) const noexcept {
    const Slot* s = datagram_slot(socket);
    return s == nullptr ? DatagramStats{} : s->stats;
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
    return wheel_.arm(now(), delay, handler);
}

void UringReactor::cancel_timer(TimerId timer) noexcept {
    wheel_.cancel(timer);
}

int UringReactor::run_once(core::Millis max_wait) {
    const bool idle = deliveries_.empty() && deferred_errors_.empty() &&
                      datagram_deliveries_.empty() && (starved_.empty() || !datagram_buffer_free());
    core::Millis wait = idle ? max_wait : core::Millis{0};
    if (const auto next = wheel_.next_expiry(now())) {
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
    iterating_ = true;

    unsigned head = 0;
    unsigned seen = 0;
    io_uring_cqe* cqe = nullptr;
    io_uring_for_each_cqe(&ring_, head, cqe) {
        ++seen;
        dispatch(*cqe);
    }
    io_uring_cq_advance(&ring_, seen);
    run_deferred();
    const int dispatched = static_cast<int>(seen) + static_cast<int>(wheel_.tick_to(now_));
    iterating_ = false;
    return dispatched;
}

void UringReactor::dispatch(const io_uring_cqe& cqe) noexcept {
    const std::uint64_t token = io_uring_cqe_get_data64(&cqe);
    const auto op = static_cast<Op>(token & 0xFFU);
    if (op == Op::Probe) {
        return;
    }
    if (op == Op::SendTo) {
        on_send_to(token >> kSendIndexShift, cqe);
        return;
    }
    const auto fd = static_cast<int>(token >> kSlotShift);
    const auto gen = static_cast<std::uint32_t>((token >> kGenShift) & 0xFFFFFFFFU);
    const bool more = (cqe.flags & IORING_CQE_F_MORE) != 0;
    std::optional<std::uint16_t> bid;
    if ((cqe.flags & IORING_CQE_F_BUFFER) != 0) {
        bid = static_cast<std::uint16_t>(cqe.flags >> IORING_CQE_BUFFER_SHIFT);
    }

    Slot& s = slots_[static_cast<std::size_t>(fd)];
    if (s.gen != gen || s.kind == Kind::Free) {
        if (bid && op == Op::DatagramRecv) {
            return_datagram_buffer(*bid);
        } else if (bid) {
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
    case Op::Recv: {
        std::span<const std::byte> data;
        if (bid && cqe.res > 0) {
            data = {buf_mem_.get() + (static_cast<std::size_t>(*bid) * kBufSize),
                    static_cast<std::size_t>(cqe.res)};
        }
        on_recv(fd, s, cqe.res, data);
        if (bid) {
            return_buffer(*bid);
        }
        break;
    }
    case Op::DatagramRecv:
        on_datagram_recv(fd, s, cqe.res, bid, more);
        break;
    case Op::SendTo:
    case Op::Probe:
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
            try {
                s.parked.append(data, pool_);
            } catch (const std::bad_alloc&) {
                fail(fd, s);
                deferred_errors_.push_back({.conn = ConnId{.fd = fd, .gen = gen}, .err = ENOMEM});
                return;
            }
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
    if (s.shut_pending) {
        s.shut_pending = false;
        static_cast<void>(::shutdown(fd, SHUT_WR));
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

void UringReactor::on_datagram_recv(int fd, Slot& s, int res, std::optional<std::uint16_t> bid,
                                    bool more) noexcept {
    if (!more) {
        s.recv_armed = false;
    }
    const std::uint32_t gen = s.gen;
    if (bid) {
        if (s.closing || res < 0) {
            return_datagram_buffer(*bid);
        } else {
            take_datagram(s, *bid, res);
        }
    } else if (res == -ENOBUFS) {
        // Re-arming now would fail the same way at once; rearm_starved waits for a buffer.
        ++s.stats.ring_exhausted;
        if (!s.closing && !s.starved) {
            s.starved = true;
            starved_.push_back(DatagramId{.fd = fd, .gen = gen});
        }
    } else if (res < 0 && res != -ECANCELED && !s.closing) {
        if (s.receiving && s.held_head < 0) {
            s.receiving = false;
            s.dgram->on_error(-res);
        } else {
            s.held_error = -res;
        }
    }
    if (alive(fd, gen) && s.receiving && !s.recv_armed && !s.starved && s.held_head < 0 &&
        s.held_error == 0) {
        arm_datagram_recv(fd, s);
    }
}

void UringReactor::take_datagram(Slot& s, std::uint16_t bid, int res) noexcept {
    const Datagram d = parse_datagram(bid, res);
    if (d.truncated) {
        ++s.stats.truncated;
        return_datagram_buffer(bid);
        return;
    }
    // Behind datagrams already held, a new one waits its turn even once receiving resumes.
    if (!s.receiving || s.held_head >= 0) {
        hold(s, bid, res);
        return;
    }
    ++s.stats.received;
    s.dgram->on_datagram(d.from, d.payload);
    return_datagram_buffer(bid);
}

void UringReactor::on_send_to(std::size_t index, const io_uring_cqe& cqe) noexcept {
    PendingSend& p = *send_pool_[index];
    Slot& s = slots_[static_cast<std::size_t>(p.fd)];
    if ((cqe.flags & IORING_CQE_F_NOTIF) != 0) {
        // The kernel is done with the payload. A copy anyway (loopback, or a device that
        // cannot send from user pages) costs more than a plain send (measured: 1,640 against
        // 1,440 ns per 1,200-byte datagram on loopback), so the socket stops asking.
        if ((static_cast<std::uint32_t>(cqe.res) & IORING_NOTIF_USAGE_ZC_COPIED) != 0) {
            ++s.stats.zero_copy_copied;
            s.zero_copy = false;
        }
    } else if (!s.closing && cqe.res >= 0) {
        ++s.stats.sent;
    } else if (!s.closing && cqe.res != -ECANCELED && p.zero_copy) {
        // A zero-copy send can fail where a plain one of the same datagram succeeds, as a burst
        // of them did on the runners' 6.8 kernel. The datagram goes again as a plain send, and
        // the socket sends plainly from now on; only a plain failure is the caller's to hear of.
        s.zero_copy = false;
        submit_send(static_cast<std::uint32_t>(index), s, false);
    } else if (!s.closing && cqe.res != -ECANCELED) {
        ++s.stats.send_errors;
        s.dgram->on_send_error(p.to, -cqe.res);
    }
    // A zero-copy send reports twice: its result, then the notification that frees the payload.
    if ((cqe.flags & IORING_CQE_F_MORE) != 0) {
        return;
    }
    assert(s.in_flight > 0 && p.pending > 0);
    --s.in_flight;
    if (--p.pending == 0) {
        free_sends_.push_back(static_cast<std::uint32_t>(index));
        --sends_in_flight_;
    }
    if (s.closing && s.in_flight == 0) {
        finalize(s);
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

    deliver_held_datagrams();
    rearm_starved();

    reporting_.swap(deferred_errors_);
    for (const DeferredError& e : reporting_) {
        if (Slot* s = stream_slot(e.conn)) {
            s->stream->on_error(e.err);
        }
    }
    reporting_.clear();
}

void UringReactor::deliver_held_datagrams() noexcept {
    delivering_datagrams_.swap(datagram_deliveries_);
    for (const DatagramId socket : delivering_datagrams_) {
        Slot* s = datagram_slot(socket);
        if (s == nullptr) {
            continue;
        }
        s->delivery_queued = false;
        while (s->receiving && s->held_head >= 0) {
            // Off the list before the callback, which may close the socket and release the rest.
            const std::uint16_t bid = pop_held(*s);
            const Datagram d = parse_datagram(bid, held_len_[bid]);
            ++s->stats.received;
            s->dgram->on_datagram(d.from, d.payload);
            return_datagram_buffer(bid);
            if (!alive(socket.fd, socket.gen)) {
                break;
            }
        }
        if (!alive(socket.fd, socket.gen) || !s->receiving || s->held_head >= 0) {
            continue;
        }
        if (s->held_error != 0) {
            s->receiving = false;
            s->dgram->on_error(std::exchange(s->held_error, 0));
        } else if (!s->recv_armed && !s->starved) {
            arm_datagram_recv(socket.fd, *s);
        }
    }
    delivering_datagrams_.clear();
}

void UringReactor::rearm_starved() noexcept {
    // Every buffer the kernel filled this iteration is back by now, so only held datagrams can
    // keep the ring empty; until one of those is delivered or dropped, re-arming would spin.
    if (starved_.empty() || !datagram_buffer_free()) {
        return;
    }
    for (const DatagramId socket : starved_) {
        Slot* s = datagram_slot(socket);
        if (s == nullptr || !s->starved) {
            continue;
        }
        s->starved = false;
        if (s->receiving && !s->recv_armed && s->held_head < 0) {
            arm_datagram_recv(socket.fd, *s);
        }
    }
    starved_.clear();
}

} // namespace net::detail
