#include "epoll_reactor.hpp"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <algorithm>
#include <cerrno>

namespace net::detail {

namespace {

// Level-triggered: a handler that stops early leaves the event pending instead of lost.
constexpr int kMaxEvents = 256;
// 4 x 64 KiB per wakeup bounds how long one busy connection can hold the loop.
constexpr int kMaxReadsPerEvent = 4;
constexpr int kMaxAcceptsPerEvent = 64;
constexpr core::Millis kAcceptRetry{100};
// The largest legitimate response is a rewritten media playlist for a 6 h video:
// 5400 segments x ~400 bytes of presigned URL = 2.2 MB. Anything above 4 MiB is a peer that
// stopped reading.
constexpr std::size_t kMaxSendQueue = std::size_t{4} * 1024 * 1024;
constexpr std::size_t kMaxIov = 16;

std::uint64_t make_token(int fd, std::uint32_t gen) noexcept {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(fd)) << 32U) | gen;
}

int socket_error(int fd) noexcept {
    int err = 0;
    socklen_t len = sizeof err;
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
        return errno;
    }
    return err != 0 ? err : EIO;
}

bool is_transient_accept_error(int err) noexcept {
    // accept(2): these are errors of the connection being accepted, not of the listener.
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
        return true;
    default:
        return false;
    }
}

} // namespace

std::expected<std::unique_ptr<EpollReactor>, int> EpollReactor::create(core::ports::IClock& clock,
                                                                       std::size_t max_fds) {
    os::UniqueFd epfd{::epoll_create1(EPOLL_CLOEXEC)};
    if (!epfd) {
        return std::unexpected(errno);
    }
    return std::make_unique<EpollReactor>(clock, std::move(epfd), max_fds);
}

EpollReactor::EpollReactor(core::ports::IClock& clock, os::UniqueFd epfd, std::size_t max_fds)
    : clock_(clock), epfd_(std::move(epfd)), slots_(max_fds), wheel_(clock.now()),
      now_(clock.now()), read_buf_(std::make_unique<ReadBuffer>()) {
    accept_retry_.self = this;
}

EpollReactor::~EpollReactor() {
    for (Slot& s : slots_) {
        s.sendq.clear(pool_);
    }
}

bool EpollReactor::alive(int fd, std::uint32_t gen) const noexcept {
    const Slot& s = slots_[static_cast<std::size_t>(fd)];
    return s.kind != Kind::Free && s.gen == gen;
}

std::uint32_t EpollReactor::wanted_events(const Slot& s) noexcept {
    std::uint32_t want = 0;
    if (s.receiving) {
        want |= EPOLLIN | EPOLLRDHUP;
    }
    if (!s.sendq.empty()) {
        want |= EPOLLOUT;
    }
    return want;
}

void EpollReactor::update_events(int fd, Slot& s) noexcept {
    if (!s.in_set) {
        return;
    }
    const std::uint32_t want = wanted_events(s);
    if (want == s.events) {
        return;
    }
    epoll_event ev{.events = want, .data = {.u64 = make_token(fd, s.gen)}};
    if (::epoll_ctl(epfd_.get(), EPOLL_CTL_MOD, fd, &ev) == 0) {
        s.events = want;
    } else {
        fail(fd, s, errno);
    }
}

void EpollReactor::remove_from_set(int fd, Slot& s) noexcept {
    if (s.in_set) {
        // Must precede close: a descriptor closed while registered stays in the interest set
        // as long as another fd references the file, and keeps producing events.
        static_cast<void>(::epoll_ctl(epfd_.get(), EPOLL_CTL_DEL, fd, nullptr));
        s.in_set = false;
        s.events = 0;
    }
}

void EpollReactor::release(Slot& s) noexcept {
    s.sendq.clear(pool_);
    s.owned.reset();
    s.stream = nullptr;
    s.ready = nullptr;
    s.acceptor = nullptr;
    s.receiving = s.failed = s.hung_up = s.eof = s.accept_paused = false;
    s.kind = Kind::Free;
    ++s.gen;
}

// From here on the connection only waits for begin_close. Bytes sent later are dropped, so the
// peer never receives what was queued behind bytes that were lost.
void EpollReactor::disable(int fd, Slot& s) noexcept {
    remove_from_set(fd, s);
    s.failed = true;
    s.receiving = false;
}

void EpollReactor::fail(int fd, Slot& s, int err) noexcept {
    disable(fd, s);
    deferred_errors_.push_back({.conn = ConnId{.fd = fd, .gen = s.gen}, .err = err});
}

std::expected<void, int> EpollReactor::listen(os::UniqueFd listener, IAcceptHandler& handler) {
    const int fd = listener.get();
    if (fd < 0 || static_cast<std::size_t>(fd) >= slots_.size()) {
        return std::unexpected(EMFILE);
    }
    Slot& s = slots_[static_cast<std::size_t>(fd)];
    epoll_event ev{.events = EPOLLIN, .data = {.u64 = make_token(fd, s.gen)}};
    if (::epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, fd, &ev) != 0) {
        return std::unexpected(errno);
    }
    s.kind = Kind::Listener;
    s.in_set = true;
    s.events = EPOLLIN;
    s.owned = std::move(listener);
    s.acceptor = &handler;
    listeners_.push_back(fd);
    return {};
}

void EpollReactor::stop_listening() noexcept {
    for (const int fd : listeners_) {
        Slot& s = slots_[static_cast<std::size_t>(fd)];
        remove_from_set(fd, s);
        release(s);
    }
    listeners_.clear();
    cancel_timer(accept_retry_.timer);
}

std::expected<ConnId, int> EpollReactor::attach(os::UniqueFd conn, IStreamHandler& handler) {
    const int fd = conn.get();
    if (fd < 0 || static_cast<std::size_t>(fd) >= slots_.size()) {
        return std::unexpected(EMFILE);
    }
    Slot& s = slots_[static_cast<std::size_t>(fd)];
    // Registered with no interest: EPOLLERR and EPOLLHUP are always reported regardless.
    epoll_event ev{.events = 0, .data = {.u64 = make_token(fd, s.gen)}};
    if (::epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, fd, &ev) != 0) {
        return std::unexpected(errno);
    }
    s.kind = Kind::Stream;
    s.in_set = true;
    s.events = 0;
    s.owned = std::move(conn);
    s.stream = &handler;
    return ConnId{.fd = fd, .gen = s.gen};
}

void EpollReactor::start_receiving(ConnId conn) noexcept {
    Slot* s = stream_slot(conn);
    if (s == nullptr || s->eof || s->failed) {
        return;
    }
    s->receiving = true;
    if (s->hung_up) {
        // Level-triggered: back in the set, the descriptor reports its unread bytes and the
        // hangup on the next wait, and reading them ends in EOF as it would have.
        s->hung_up = false;
        epoll_event ev{.events = wanted_events(*s), .data = {.u64 = make_token(conn.fd, s->gen)}};
        if (::epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, conn.fd, &ev) != 0) {
            fail(conn.fd, *s, errno);
            return;
        }
        s->in_set = true;
        s->events = ev.events;
        return;
    }
    update_events(conn.fd, *s);
}

void EpollReactor::stop_receiving(ConnId conn) noexcept {
    Slot* s = stream_slot(conn);
    if (s == nullptr) {
        return;
    }
    s->receiving = false;
    update_events(conn.fd, *s);
}

void EpollReactor::send(ConnId conn, std::span<const std::byte> bytes) noexcept {
    Slot* s = stream_slot(conn);
    if (s == nullptr || s->failed || bytes.empty()) {
        return;
    }
    if (s->sendq.size() + bytes.size() > kMaxSendQueue) {
        fail(conn.fd, *s, ENOBUFS);
        return;
    }
    if (s->sendq.empty()) {
        const ssize_t n = ::send(conn.fd, bytes.data(), bytes.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0 && errno != EAGAIN && errno != EINTR) {
            fail(conn.fd, *s, errno);
            return;
        }
        bytes = bytes.subspan(n > 0 ? static_cast<std::size_t>(n) : 0);
        if (bytes.empty()) {
            return;
        }
    }
    s->sendq.append(bytes, pool_);
    update_events(conn.fd, *s);
}

std::size_t EpollReactor::pending_send_bytes(ConnId conn) const noexcept {
    const Slot* s = stream_slot(conn);
    return s == nullptr ? 0 : s->sendq.size();
}

void EpollReactor::begin_close(ConnId conn) noexcept {
    Slot* s = stream_slot(conn);
    if (s == nullptr) {
        return;
    }
    remove_from_set(conn.fd, *s);
    release(*s);
}

bool EpollReactor::is_quiescent(ConnId conn) const noexcept {
    return stream_slot(conn) == nullptr;
}

std::expected<void, int> EpollReactor::watch(int fd, Interest interest, IReadyHandler& handler) {
    if (fd < 0 || static_cast<std::size_t>(fd) >= slots_.size()) {
        return std::unexpected(EMFILE);
    }
    Slot& s = slots_[static_cast<std::size_t>(fd)];
    if (s.kind != Kind::Free && s.kind != Kind::Watch) {
        return std::unexpected(EEXIST);
    }
    std::uint32_t want = 0;
    if (has(interest, Interest::Read)) {
        want |= EPOLLIN;
    }
    if (has(interest, Interest::Write)) {
        want |= EPOLLOUT;
    }
    if (want == 0) {
        // HUP and ERR are reported whatever the mask, so no interest has to mean out of the
        // set, or a hung-up descriptor is reported on every wait.
        remove_from_set(fd, s);
    } else {
        epoll_event ev{.events = want, .data = {.u64 = make_token(fd, s.gen)}};
        if (::epoll_ctl(epfd_.get(), s.in_set ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, fd, &ev) != 0) {
            return std::unexpected(errno);
        }
        s.in_set = true;
        s.events = want;
    }
    s.kind = Kind::Watch;
    s.ready = &handler;
    return {};
}

void EpollReactor::unwatch(int fd) noexcept {
    if (fd < 0 || static_cast<std::size_t>(fd) >= slots_.size()) {
        return;
    }
    Slot& s = slots_[static_cast<std::size_t>(fd)];
    if (s.kind != Kind::Watch) {
        return;
    }
    remove_from_set(fd, s);
    release(s);
}

TimerId EpollReactor::arm_timer(core::Millis delay, ITimerHandler& handler) {
    return wheel_.arm(now_, delay, handler);
}

void EpollReactor::cancel_timer(TimerId timer) noexcept {
    wheel_.cancel(timer);
}

int EpollReactor::run_once(core::Millis max_wait) {
    core::Millis wait = deferred_errors_.empty() ? max_wait : core::Millis{0};
    if (const auto next = wheel_.next_expiry(now_)) {
        wait = std::min(wait, *next);
    }
    std::array<epoll_event, kMaxEvents> events{};
    const auto timeout_ms =
        static_cast<int>(std::clamp<core::Millis::rep>(wait.count(), 0, 60'000));
    // EINTR is the only error a valid epoll descriptor can return here.
    const int n = std::max(0, ::epoll_wait(epfd_.get(), events.data(), kMaxEvents, timeout_ms));
    now_ = clock_.now();
    for (int i = 0; i < n; ++i) {
        const auto& ev = events[static_cast<std::size_t>(i)];
        dispatch(ev.data.u64, ev.events);
    }
    run_deferred();
    return n + static_cast<int>(wheel_.tick_to(now_));
}

void EpollReactor::run_deferred() noexcept {
    // Errors reported by one callback can queue more; swap so the loop is bounded.
    std::vector<DeferredError> errors;
    errors.swap(deferred_errors_);
    for (const DeferredError& e : errors) {
        if (Slot* s = stream_slot(e.conn)) {
            s->stream->on_error(e.err);
        }
    }
}

void EpollReactor::dispatch(std::uint64_t token, std::uint32_t events) noexcept {
    const auto fd = static_cast<int>(token >> 32U);
    const auto gen = static_cast<std::uint32_t>(token & 0xFFFFFFFFU);
    if (fd < 0 || static_cast<std::size_t>(fd) >= slots_.size() || !alive(fd, gen)) {
        return;
    }
    Slot& s = slots_[static_cast<std::size_t>(fd)];
    switch (s.kind) {
    case Kind::Stream:
        on_stream_event(fd, s, events);
        return;
    case Kind::Watch: {
        Interest ready = Interest::None;
        // Errors are reported as both directions so the owning library discovers them on
        // whichever operation it attempts next.
        if ((events & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0) {
            ready = ready | Interest::Read;
        }
        if ((events & (EPOLLOUT | EPOLLERR)) != 0) {
            ready = ready | Interest::Write;
        }
        s.ready->on_ready(ready);
        return;
    }
    case Kind::Listener:
        on_listener_event(fd, s);
        return;
    case Kind::Free:
        return;
    }
}

void EpollReactor::on_stream_event(int fd, Slot& s, std::uint32_t events) noexcept {
    const std::uint32_t gen = s.gen;
    if ((events & EPOLLERR) != 0) {
        const int err = socket_error(fd);
        disable(fd, s);
        s.stream->on_error(err);
        return;
    }
    if ((events & EPOLLIN) != 0 && s.receiving) {
        read_ready(fd, s);
        if (!alive(fd, gen)) {
            return;
        }
    }
    if ((events & EPOLLHUP) != 0) {
        if (s.receiving) {
            // Unread data may remain; it is read on the next wakeup and ends in EOF.
            return;
        }
        // Both directions are gone, but that is not EOF yet: bytes the peer sent before it
        // hung up may still be unread. Queued bytes can never leave, and trying reports why
        // instead of leaving them stranded.
        if (!s.sendq.empty()) {
            flush(fd, s);
            if (!alive(fd, gen) || s.failed) {
                return;
            }
        }
        remove_from_set(fd, s);
        s.hung_up = true;
        return;
    }
    if ((events & EPOLLOUT) != 0 && !s.sendq.empty()) {
        flush(fd, s);
    }
}

void EpollReactor::read_ready(int fd, Slot& s) noexcept {
    const std::uint32_t gen = s.gen;
    auto& buf = *read_buf_;
    for (int i = 0; i < kMaxReadsPerEvent; ++i) {
        const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n > 0) {
            s.stream->on_data({buf.data(), static_cast<std::size_t>(n)});
            if (!alive(fd, gen) || !s.receiving) {
                return;
            }
            if (static_cast<std::size_t>(n) < buf.size()) {
                return;
            }
            continue;
        }
        if (n == 0) {
            s.eof = true;
            s.receiving = false;
            update_events(fd, s);
            s.stream->on_peer_eof();
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN) {
            const int err = errno;
            disable(fd, s);
            s.stream->on_error(err);
        }
        return;
    }
}

void EpollReactor::flush(int fd, Slot& s) noexcept {
    std::array<std::span<const std::byte>, kMaxIov> spans{};
    std::array<iovec, kMaxIov> iov{};
    while (!s.sendq.empty()) {
        const std::size_t count = s.sendq.gather(spans);
        for (std::size_t i = 0; i < count; ++i) {
            // iovec is shared with readv, hence non-const; sendmsg never writes through it.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
            iov[i] = {.iov_base = const_cast<std::byte*>(spans[i].data()),
                      .iov_len = spans[i].size()};
        }
        msghdr msg{};
        msg.msg_iov = iov.data();
        msg.msg_iovlen = count;
        const ssize_t n = ::sendmsg(fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN) {
                const int err = errno;
                disable(fd, s);
                s.stream->on_error(err);
            }
            return;
        }
        s.sendq.consume(static_cast<std::size_t>(n), pool_);
    }
    update_events(fd, s);
    s.stream->on_writable();
}

void EpollReactor::on_listener_event(int fd, Slot& s) noexcept {
    const std::uint32_t gen = s.gen;
    for (int i = 0; i < kMaxAcceptsPerEvent; ++i) {
        const int conn = ::accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (conn >= 0) {
            s.acceptor->on_accept(os::UniqueFd{conn});
            if (!alive(fd, gen)) {
                return;
            }
            continue;
        }
        const int err = errno;
        if (err == EINTR || is_transient_accept_error(err)) {
            continue;
        }
        if (err == EMFILE || err == ENFILE || err == ENOBUFS || err == ENOMEM) {
            // The pending connection stays in the backlog and the listener stays readable,
            // so retrying now would spin. Stop watching it and look again shortly.
            pause_accepting(fd, s);
        }
        return;
    }
}

void EpollReactor::pause_accepting(int fd, Slot& s) noexcept {
    epoll_event ev{.events = 0, .data = {.u64 = make_token(fd, s.gen)}};
    static_cast<void>(::epoll_ctl(epfd_.get(), EPOLL_CTL_MOD, fd, &ev));
    s.events = 0;
    s.accept_paused = true;
    wheel_.cancel(accept_retry_.timer);
    accept_retry_.timer = wheel_.arm(now_, kAcceptRetry, accept_retry_);
}

void EpollReactor::AcceptRetry::on_timeout() noexcept {
    for (const int fd : self->listeners_) {
        Slot& s = self->slots_[static_cast<std::size_t>(fd)];
        if (!s.accept_paused) {
            continue;
        }
        epoll_event ev{.events = EPOLLIN, .data = {.u64 = make_token(fd, s.gen)}};
        if (::epoll_ctl(self->epfd_.get(), EPOLL_CTL_MOD, fd, &ev) == 0) {
            s.events = EPOLLIN;
            s.accept_paused = false;
        }
    }
}

} // namespace net::detail
