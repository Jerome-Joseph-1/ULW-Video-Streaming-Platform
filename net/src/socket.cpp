#include "net/socket.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <array>
#include <cerrno>

namespace net {

namespace {

constexpr int kBacklog = 1024;

std::expected<void, int> set_int(int fd, int level, int name, int value) noexcept {
    if (::setsockopt(fd, level, name, &value, sizeof value) != 0) {
        return std::unexpected(errno);
    }
    return {};
}

} // namespace

std::expected<os::UniqueFd, int> listen_tcp(const ListenOptions& options) {
    bool v6 = true;
    os::UniqueFd fd{::socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    if (!fd && errno == EAFNOSUPPORT) {
        v6 = false;
        fd = os::UniqueFd{::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    }
    if (!fd) {
        return std::unexpected(errno);
    }
    if (auto r = set_int(fd.get(), SOL_SOCKET, SO_REUSEADDR, 1); !r) {
        return std::unexpected(r.error());
    }
    if (options.reuse_port) {
        if (auto r = set_int(fd.get(), SOL_SOCKET, SO_REUSEPORT, 1); !r) {
            return std::unexpected(r.error());
        }
    }
    int rc = 0;
    if (v6) {
        if (auto r = set_int(fd.get(), IPPROTO_IPV6, IPV6_V6ONLY, 0); !r) {
            return std::unexpected(r.error());
        }
        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(options.port);
        addr.sin6_addr = options.loopback_only ? in6addr_loopback : in6addr_any;
        rc = ::bind(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr);
    } else {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(options.port);
        addr.sin_addr.s_addr = htonl(options.loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
        rc = ::bind(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr);
    }
    if (rc != 0 || ::listen(fd.get(), kBacklog) != 0) {
        return std::unexpected(errno);
    }
    return fd;
}

std::expected<std::uint16_t, int> local_port(int fd) noexcept {
    sockaddr_storage addr{};
    socklen_t len = sizeof addr;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return std::unexpected(errno);
    }
    if (addr.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&addr)->sin6_port);
    }
    return ntohs(reinterpret_cast<const sockaddr_in*>(&addr)->sin_port);
}

std::expected<void, int> tune_connection(int fd) noexcept {
    // Keepalive 60 s idle, then 3 probes 10 s apart: a vanished peer is found in 90 s.
    // TCP_USER_TIMEOUT bounds how long sent data may sit unacknowledged, which keepalive
    // alone does not cover while the send queue is non-empty.
    constexpr int kKeepIdle = 60;
    constexpr int kKeepIntvl = 10;
    constexpr int kKeepCnt = 3;
    constexpr int kUserTimeoutMs = 20'000;
    struct Option {
        int level;
        int name;
        int value;
    };
    const std::array<Option, 6> options{{
        {.level = IPPROTO_TCP, .name = TCP_NODELAY, .value = 1},
        {.level = SOL_SOCKET, .name = SO_KEEPALIVE, .value = 1},
        {.level = IPPROTO_TCP, .name = TCP_KEEPIDLE, .value = kKeepIdle},
        {.level = IPPROTO_TCP, .name = TCP_KEEPINTVL, .value = kKeepIntvl},
        {.level = IPPROTO_TCP, .name = TCP_KEEPCNT, .value = kKeepCnt},
        {.level = IPPROTO_TCP, .name = TCP_USER_TIMEOUT, .value = kUserTimeoutMs},
    }};
    for (const Option& o : options) {
        if (auto r = set_int(fd, o.level, o.name, o.value); !r) {
            return r;
        }
    }
    return {};
}

} // namespace net
