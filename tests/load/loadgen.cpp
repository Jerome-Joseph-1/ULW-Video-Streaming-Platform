// Load generator for the echo server. Plain epoll on purpose, so it shares no code with the
// reactors it measures.
//
//   ulw_loadgen --port P --connections N --seconds S --mode pingpong|idle|noread
//
// pingpong: each connection sends a 64-byte message and waits for the exact echo, repeatedly.
// idle:     each connection sends one message, reads its echo, then stays silent.
// noread:   each connection writes as fast as the kernel accepts and never reads.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <print>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

enum class Mode { PingPong, Idle, NoRead };

struct Args {
    std::uint16_t port = 7000;
    std::uint64_t connections = 500;
    std::uint64_t seconds = 60;
    Mode mode = Mode::PingPong;
};

constexpr std::size_t kMessage = 64;

struct Conn {
    int fd = -1;
    std::size_t sent = 0;
    std::size_t received = 0;
    std::uint64_t round_trips = 0;
    bool open = false;
};

bool parse(int argc, char** argv, Args& args) {
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string_view flag = argv[i];
        const std::string_view v = argv[i + 1];
        auto num = [&](auto& out) {
            const auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
            return ec == std::errc{} && p == v.data() + v.size();
        };
        bool ok = false;
        if (flag == "--port") {
            ok = num(args.port);
        } else if (flag == "--connections") {
            ok = num(args.connections);
        } else if (flag == "--seconds") {
            ok = num(args.seconds);
        } else if (flag == "--mode") {
            ok = true;
            if (v == "pingpong") {
                args.mode = Mode::PingPong;
            } else if (v == "idle") {
                args.mode = Mode::Idle;
            } else if (v == "noread") {
                args.mode = Mode::NoRead;
            } else {
                ok = false;
            }
        }
        if (!ok) {
            return false;
        }
    }
    return argc % 2 == 1;
}

int open_conn(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0 &&
        errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }
    return fd;
}

} // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse(argc, argv, args)) {
        std::println(stderr, "usage: ulw_loadgen --port P --connections N --seconds S "
                             "--mode pingpong|idle|noread");
        return 2;
    }
    rlimit lim{};
    ::getrlimit(RLIMIT_NOFILE, &lim);
    lim.rlim_cur = lim.rlim_max;
    ::setrlimit(RLIMIT_NOFILE, &lim);

    const int ep = ::epoll_create1(EPOLL_CLOEXEC);
    std::vector<Conn> conns(args.connections);
    std::array<std::byte, kMessage> msg{};
    for (std::size_t i = 0; i < msg.size(); ++i) {
        msg[i] = static_cast<std::byte>('a' + (i % 26));
    }
    std::uint64_t connect_failures = 0;
    for (std::size_t i = 0; i < conns.size(); ++i) {
        conns[i].fd = open_conn(args.port);
        if (conns[i].fd < 0) {
            ++connect_failures;
            continue;
        }
        conns[i].open = true;
        // A client that never reads must not ask to be woken for input either.
        const std::uint32_t interest =
            args.mode == Mode::NoRead ? EPOLLOUT : EPOLLIN | EPOLLOUT | EPOLLRDHUP;
        epoll_event ev{.events = interest, .data = {.u64 = i}};
        ::epoll_ctl(ep, EPOLL_CTL_ADD, conns[i].fd, &ev);
    }

    std::uint64_t mismatches = 0;
    std::uint64_t closed_by_peer = 0;
    std::uint64_t errors = 0;
    std::uint64_t bytes_written = 0;
    std::array<std::byte, 65536> buf{};
    std::array<std::byte, 65536> junk{};
    std::array<epoll_event, 512> events{};
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(args.seconds);

    while (std::chrono::steady_clock::now() < deadline) {
        const int n = ::epoll_wait(ep, events.data(), static_cast<int>(events.size()), 100);
        for (int k = 0; k < n; ++k) {
            const auto& ev = events[static_cast<std::size_t>(k)];
            Conn& c = conns[ev.data.u64];
            if (!c.open) {
                continue;
            }
            auto drop = [&](std::uint64_t& counter) {
                ++counter;
                ::epoll_ctl(ep, EPOLL_CTL_DEL, c.fd, nullptr);
                ::close(c.fd);
                c.open = false;
            };
            if ((ev.events & EPOLLERR) != 0) {
                drop(errors);
                continue;
            }
            if ((ev.events & EPOLLIN) != 0 && args.mode != Mode::NoRead) {
                const ssize_t r = ::recv(c.fd, buf.data(), buf.size(), 0);
                if (r == 0) {
                    drop(closed_by_peer);
                    continue;
                }
                if (r < 0 && errno != EAGAIN) {
                    drop(errors);
                    continue;
                }
                for (ssize_t j = 0; j < r; ++j) {
                    if (buf[static_cast<std::size_t>(j)] !=
                        msg[(c.received + static_cast<std::size_t>(j)) % kMessage]) {
                        ++mismatches;
                        break;
                    }
                }
                c.received += r > 0 ? static_cast<std::size_t>(r) : 0;
                if (args.mode == Mode::PingPong && c.received == c.sent) {
                    ++c.round_trips;
                    epoll_event mod{.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP,
                                    .data = {.u64 = ev.data.u64}};
                    ::epoll_ctl(ep, EPOLL_CTL_MOD, c.fd, &mod);
                }
            }
            if ((ev.events & EPOLLOUT) != 0) {
                if (args.mode == Mode::NoRead) {
                    const ssize_t w = ::send(c.fd, junk.data(), junk.size(), MSG_NOSIGNAL);
                    if (w > 0) {
                        bytes_written += static_cast<std::size_t>(w);
                    } else if (errno != EAGAIN) {
                        drop(errors);
                    }
                    continue;
                }
                if (c.sent == c.received && (args.mode == Mode::PingPong || c.sent == 0)) {
                    const ssize_t w = ::send(c.fd, msg.data(), msg.size(), MSG_NOSIGNAL);
                    if (w != static_cast<ssize_t>(msg.size())) {
                        drop(errors);
                        continue;
                    }
                    c.sent += msg.size();
                }
                // Only wake for input until the echo is back.
                epoll_event mod{.events = EPOLLIN | EPOLLRDHUP, .data = {.u64 = ev.data.u64}};
                ::epoll_ctl(ep, EPOLL_CTL_MOD, c.fd, &mod);
            }
        }
    }

    std::uint64_t round_trips = 0;
    std::uint64_t still_open = 0;
    for (const Conn& c : conns) {
        round_trips += c.round_trips;
        still_open += c.open ? 1 : 0;
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::println("connections={} open_at_end={} connect_failures={} closed_by_peer={} errors={} "
                 "mismatches={} round_trips={} rt_per_sec={:.0f} bytes_written={}",
                 args.connections, still_open, connect_failures, closed_by_peer, errors, mismatches,
                 round_trips, static_cast<double>(round_trips) / secs, bytes_written);
    return mismatches == 0 && errors == 0 ? 0 : 1;
}
