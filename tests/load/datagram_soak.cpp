// Many low-rate UDP peers against one echoing socket, each peer on its own socket with its own
// nonce, while the process samples its RSS. Both sides run on reactors of the same kind: the
// echo server on a thread of its own, the peers on the main thread.

#include "net/reactor_factory.hpp"
#include "net/socket.hpp"
#include "os/limits.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include <sys/socket.h>

#include <array>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <future>
#include <memory>
#include <print>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using net::DatagramId;
using net::SocketAddr;

struct Args {
    net::ReactorKind reactor = net::ReactorKind::IoUring;
    std::uint64_t peers = 5000;
    std::uint64_t interval_ms = 1000;
    std::uint64_t duration_s = 600;
    std::uint64_t sample_s = 10;
    std::uint64_t burst = 1;
    // Zero leaves RSS unchecked; the tool then only reports it.
    std::uint64_t max_rss_bytes_per_datagram = 0;
};

template <class T> bool parse_number(std::string_view text, T& out) {
    const char* const first = std::to_address(text.begin());
    const char* const last = std::to_address(text.end());
    const auto [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && ptr == last;
}

bool parse(std::span<char*> argv, Args& args) {
    for (std::size_t i = 1; i + 1 < argv.size(); i += 2) {
        const std::string_view flag = argv[i];
        const std::string_view value = argv[i + 1];
        bool ok = false;
        if (flag == "--reactor") {
            const auto kind = net::parse_reactor_kind(value);
            ok = kind.has_value();
            if (ok) {
                args.reactor = *kind;
            }
        } else if (flag == "--peers") {
            ok = parse_number(value, args.peers) && args.peers > 0;
        } else if (flag == "--interval-ms") {
            ok = parse_number(value, args.interval_ms) && args.interval_ms >= 100;
        } else if (flag == "--duration-s") {
            ok = parse_number(value, args.duration_s) && args.duration_s > 0;
        } else if (flag == "--sample-s") {
            ok = parse_number(value, args.sample_s) && args.sample_s > 0;
        } else if (flag == "--burst") {
            ok = parse_number(value, args.burst) && args.burst > 0;
        } else if (flag == "--max-rss-bytes-per-datagram") {
            ok = parse_number(value, args.max_rss_bytes_per_datagram);
        }
        if (!ok) {
            return false;
        }
    }
    return argv.size() % 2 == 1;
}

std::uint64_t rss_kb() {
    std::FILE* f = std::fopen("/proc/self/status", "re");
    if (f == nullptr) {
        return 0;
    }
    std::array<char, 4096> buf{};
    const std::size_t n = std::fread(buf.data(), 1, buf.size() - 1, f);
    static_cast<void>(std::fclose(f));
    const std::string_view status(buf.data(), n);
    constexpr std::string_view kKey = "VmRSS:";
    const auto at = status.find(kKey);
    if (at == std::string_view::npos) {
        return 0;
    }
    auto rest = status.substr(at + kKey.size());
    rest.remove_prefix(std::min(rest.find_first_not_of(" \t"), rest.size()));
    std::uint64_t kb = 0;
    std::from_chars(rest.data(), rest.data() + rest.size(), kb);
    return kb;
}

struct Wire {
    std::uint64_t nonce = 0;
    std::uint64_t seq = 0;
};

class Echo final : public net::IDatagramHandler {
public:
    net::IReactor* reactor = nullptr;
    DatagramId id;
    std::uint64_t refused = 0;
    std::uint64_t errors = 0;

    void on_datagram(SocketAddr from, net::BorrowedBytes payload) noexcept override {
        if (!reactor->send_to(id, from, payload)) {
            ++refused;
        }
    }
    void on_send_error(SocketAddr /*to*/, int /*err*/) noexcept override { ++errors; }
    void on_error(int /*err*/) noexcept override {
        ++errors;
        reactor->start_receiving_datagrams(id);
    }
};

struct ServerReport {
    net::DatagramStats stats;
    std::uint64_t refused = 0;
    std::uint64_t errors = 0;
};

// Runs on its own thread: an io_uring reactor belongs to the thread that created it.
void serve(net::ReactorKind kind, std::size_t max_fds, std::promise<SocketAddr>& ready,
           const std::atomic<bool>& stop, ServerReport& report) {
    os::SystemClock clock;
    auto reactor = net::make_reactor(kind, clock, max_fds);
    auto fd = net::bind_udp(SocketAddr::loopback(net::AddrFamily::V4, 0));
    if (!reactor || !fd) {
        ready.set_value({});
        return;
    }
    // One tick sends peers / (interval / 100 ms) datagrams at once, 500 for the defaults, while
    // a default receive buffer holds 256. UDP has no autotuning to spoil, and the kernel caps
    // the request at net.core.rmem_max.
    const int rcvbuf = 4 * 1024 * 1024;
    static_cast<void>(::setsockopt(fd->get(), SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf));
    const SocketAddr addr = *net::local_addr(fd->get());
    Echo echo;
    echo.reactor = reactor->get();
    auto id = (*reactor)->attach_datagram(std::move(*fd), echo);
    if (!id) {
        ready.set_value({});
        return;
    }
    echo.id = *id;
    (*reactor)->start_receiving_datagrams(*id);
    ready.set_value(addr);
    while (!stop.load(std::memory_order_relaxed)) {
        (*reactor)->run_once(core::Millis{100});
    }
    report = {
        .stats = (*reactor)->datagram_stats(*id), .refused = echo.refused, .errors = echo.errors};
}

class Fleet;

class PeerSocket final : public net::IDatagramHandler {
public:
    Fleet* fleet = nullptr;
    DatagramId id;
    std::uint64_t nonce = 0;
    std::uint64_t next_seq = 0;
    std::uint64_t echoed = 0;

    void on_datagram(SocketAddr from, net::BorrowedBytes payload) noexcept override;
    void on_send_error(SocketAddr to, int err) noexcept override;
    void on_error(int err) noexcept override;
};

class Fleet final : public net::ITimerHandler {
public:
    Fleet(net::IReactor& reactor, SocketAddr server, const Args& args)
        : reactor_(reactor), server_(server), args_(args), slices_(args.interval_ms / kTickMs),
          slices_per_fire_(std::max<std::uint64_t>(
              1, kSendBudget / std::max<std::uint64_t>(1, args.peers / slices_ * args.burst))),
          peers_(args.peers) {}

    [[nodiscard]] bool open() {
        os::SystemRandom random;
        for (PeerSocket& p : peers_) {
            auto fd = net::bind_udp(SocketAddr::loopback(net::AddrFamily::V4, 0));
            if (!fd) {
                std::println(stderr, "bind_udp: {}", std::strerror(fd.error()));
                return false;
            }
            auto id = reactor_.attach_datagram(std::move(*fd), p);
            if (!id) {
                std::println(stderr, "attach_datagram: {}", std::strerror(id.error()));
                return false;
            }
            p.fleet = this;
            p.id = *id;
            random.fill(std::as_writable_bytes(std::span(&p.nonce, 1)));
            reactor_.start_receiving_datagrams(*id);
        }
        return true;
    }

    // The first fire sends as many slices as any fire ever will, so the reactors' send pools
    // reach their working set before the first RSS sample and later growth is a leak.
    void start() {
        next_slice_at_ = reactor_.now() + core::Millis{kTickMs} -
                         (core::Millis{kTickMs} * (slices_per_fire_ - 1));
        timer_ = reactor_.arm_timer(core::Millis{kTickMs}, *this);
    }
    void stop() { reactor_.cancel_timer(timer_); }

    // Every 100 ms one slice of the peers sends, so every peer sends once per interval and the
    // load is spread evenly across it. The wheel may fire a tick late; slices that fell due in
    // the meantime go out together rather than being skipped, but no more per fire than the
    // reactor takes in one iteration, and the rest on the next.
    void on_timeout() noexcept override {
        for (std::uint64_t n = 0; n < slices_per_fire_ && next_slice_at_ <= reactor_.now(); ++n) {
            send_slice(tick_++ % slices_);
            next_slice_at_ += core::Millis{kTickMs};
        }
        timer_ = reactor_.arm_timer(core::Millis{kTickMs}, *this);
    }

    void send_slice(std::uint64_t slice) noexcept {
        for (std::size_t i = slice; i < peers_.size(); i += slices_) {
            PeerSocket& p = peers_[i];
            for (std::uint64_t b = 0; b < args_.burst; ++b) {
                const Wire wire{.nonce = p.nonce, .seq = p.next_seq};
                if (reactor_.send_to(p.id, server_, std::as_bytes(std::span(&wire, 1)))) {
                    ++p.next_seq;
                    ++sent;
                } else {
                    ++refused;
                }
            }
        }
    }

    [[nodiscard]] const SocketAddr& server() const noexcept { return server_; }
    [[nodiscard]] std::uint64_t silent_peers() const noexcept {
        return static_cast<std::uint64_t>(
            std::ranges::count_if(peers_, [](const PeerSocket& p) { return p.echoed == 0; }));
    }

    std::uint64_t sent = 0;
    std::uint64_t refused = 0;
    std::uint64_t echoed = 0;
    std::uint64_t misattributed = 0;
    std::uint64_t errors = 0;

private:
    // The timing wheel's resolution.
    static constexpr std::uint64_t kTickMs = 100;

    net::IReactor& reactor_;
    SocketAddr server_;
    const Args& args_;
    // Just under the 1,024 sends an io_uring reactor holds in flight: one fire's sends are all
    // taken, and complete long before the next.
    static constexpr std::uint64_t kSendBudget = 1000;
    std::uint64_t slices_;
    std::uint64_t slices_per_fire_;
    std::uint64_t tick_ = 0;
    core::MonoTime next_slice_at_;
    net::TimerId timer_;
    std::vector<PeerSocket> peers_;
};

void PeerSocket::on_datagram(SocketAddr from, net::BorrowedBytes payload) noexcept {
    Wire wire;
    if (from != fleet->server() || payload.size() != sizeof wire) {
        ++fleet->misattributed;
        return;
    }
    std::memcpy(&wire, payload.data(), sizeof wire);
    if (wire.nonce != nonce || wire.seq >= next_seq) {
        ++fleet->misattributed;
        return;
    }
    ++echoed;
    ++fleet->echoed;
}

void PeerSocket::on_send_error(SocketAddr /*to*/, int /*err*/) noexcept {
    ++fleet->errors;
}

void PeerSocket::on_error(int /*err*/) noexcept {
    ++fleet->errors;
}

int run(int argc, char** argv) {
    Args args;
    if (!parse(std::span(argv, static_cast<std::size_t>(argc)), args)) {
        std::println(stderr, "usage: ulw_datagram_soak [--reactor io_uring|epoll] [--peers N] "
                             "[--interval-ms N>=100] [--duration-s N] [--sample-s N] [--burst N] "
                             "[--max-rss-bytes-per-datagram N]");
        return 2;
    }
    // One descriptor per peer plus the server's, the rings and the standard streams.
    const auto limits = os::raise_nofile_limit(65'536);
    if (!limits || limits->soft < args.peers + 64) {
        std::println(stderr, "RLIMIT_NOFILE too low for {} peers", args.peers);
        return 1;
    }

    std::promise<SocketAddr> ready;
    std::atomic<bool> stop{false};
    ServerReport report;
    std::jthread server_thread([&] { serve(args.reactor, limits->soft, ready, stop, report); });
    const SocketAddr server = ready.get_future().get();
    if (server.port == 0) {
        std::println(stderr, "server setup failed");
        return 1;
    }

    os::SystemClock clock;
    auto reactor = net::make_reactor(args.reactor, clock, limits->soft);
    if (!reactor) {
        std::println(stderr, "reactor setup: {}", std::strerror(reactor.error()));
        stop = true;
        return 1;
    }
    Fleet fleet(**reactor, server, args);
    if (!fleet.open()) {
        stop = true;
        return 1;
    }
    std::println("reactor={} peers={} interval_ms={} burst={} duration_s={}",
                 net::to_string(args.reactor), args.peers, args.interval_ms, args.burst,
                 args.duration_s);
    fleet.start();

    const auto started = (*reactor)->now();
    const auto sample_every = core::Seconds{args.sample_s};
    auto next_sample = started + sample_every;
    const auto end = started + core::Seconds{args.duration_s};
    std::uint64_t rss_first = 0;
    std::uint64_t echoed_at_first = 0;
    std::uint64_t rss_max = 0;
    std::uint64_t rss_last = 0;
    while ((*reactor)->now() < end) {
        (*reactor)->run_once(core::Millis{100});
        if ((*reactor)->now() >= next_sample) {
            next_sample += sample_every;
            rss_last = rss_kb();
            if (rss_first == 0) {
                rss_first = rss_last;
                echoed_at_first = fleet.echoed;
            }
            rss_max = std::max(rss_max, rss_last);
            const auto elapsed =
                std::chrono::duration_cast<core::Seconds>((*reactor)->now() - started);
            std::println("t={}s sent={} echoed={} refused={} misattributed={} rss_kb={}",
                         elapsed.count(), fleet.sent, fleet.echoed, fleet.refused,
                         fleet.misattributed, rss_last);
            static_cast<void>(std::fflush(stdout));
        }
    }
    fleet.stop();
    // Echoes still in flight get a second to arrive.
    const auto drain_end = (*reactor)->now() + core::Seconds{1};
    while (fleet.echoed < fleet.sent && (*reactor)->now() < drain_end) {
        (*reactor)->run_once(core::Millis{10});
    }
    stop = true;
    server_thread.join();

    const std::uint64_t lost = fleet.sent - fleet.echoed;
    const std::uint64_t growth = rss_max - rss_first;
    // Each round trip is two datagrams: the peer's and the echo.
    const std::uint64_t measured = 2 * (fleet.echoed - echoed_at_first);
    const std::uint64_t per_datagram = measured == 0 ? 0 : growth * 1024 / measured;
    std::println("summary sent={} echoed={} lost={} misattributed={} silent_peers={} "
                 "peer_refused={} peer_errors={}",
                 fleet.sent, fleet.echoed, lost, fleet.misattributed, fleet.silent_peers(),
                 fleet.refused, fleet.errors);
    std::println("server received={} sent={} truncated={} ring_exhausted={} refused={} errors={}",
                 report.stats.received, report.stats.sent, report.stats.truncated,
                 report.stats.ring_exhausted, report.refused, report.errors);
    std::println("rss_kb first_sample={} max={} last={} growth={} bytes_per_datagram={}", rss_first,
                 rss_max, rss_last, growth, per_datagram);
    const bool rss_ok = args.max_rss_bytes_per_datagram == 0 ||
                        (measured > 0 && per_datagram <= args.max_rss_bytes_per_datagram);
    // A refusal on either side means a send slot was not given back, or the fleet outran what
    // it had budgeted for; either way the run did not measure what it claims to.
    const bool refused = fleet.refused != 0 || report.refused != 0;
    return fleet.misattributed == 0 && lost == 0 && fleet.silent_peers() == 0 && !refused &&
                   fleet.errors == 0 && report.errors == 0 && rss_ok
               ? 0
               : 1;
}

} // namespace

// Formatting and allocation are all that can still throw; report it and exit.
int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        static_cast<void>(std::fputs(e.what(), stderr));
        return 1;
    } catch (...) {
        return 1;
    }
}
