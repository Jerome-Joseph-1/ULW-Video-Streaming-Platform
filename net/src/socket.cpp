#include "net/socket.hpp"

#include "core/util/parse.hpp"

#include "sockaddr.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace net {

namespace {

constexpr int kBacklog = 1024;

std::expected<void, int> set_int(int fd, int level, int name, int value) noexcept {
    if (::setsockopt(fd, level, name, &value, sizeof value) != 0) {
        return std::unexpected(errno);
    }
    return {};
}

struct Endpoint {
    sockaddr_storage addr{};
    socklen_t len = 0;
};

std::optional<Endpoint> parse_endpoint(std::string_view address) noexcept {
    const std::size_t colon = address.rfind(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view host = address.substr(0, colon);
    const auto port = core::parse_integer<std::uint16_t>(address.substr(colon + 1));
    if (!port || *port == 0) {
        return std::nullopt;
    }
    const bool bracketed = host.starts_with('[') && host.ends_with(']');
    if (bracketed) {
        host = host.substr(1, host.size() - 2);
    }
    // inet_pton wants a terminated string; INET6_ADDRSTRLEN bounds any numeric address.
    std::array<char, INET6_ADDRSTRLEN> text{};
    if (host.empty() || host.size() >= text.size()) {
        return std::nullopt;
    }
    host.copy(text.data(), host.size());
    Endpoint out;
    if (bracketed) {
        auto* v6 = reinterpret_cast<sockaddr_in6*>(&out.addr);
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(*port);
        if (::inet_pton(AF_INET6, text.data(), &v6->sin6_addr) != 1) {
            return std::nullopt;
        }
        out.len = sizeof(sockaddr_in6);
        return out;
    }
    auto* v4 = reinterpret_cast<sockaddr_in*>(&out.addr);
    v4->sin_family = AF_INET;
    v4->sin_port = htons(*port);
    if (::inet_pton(AF_INET, text.data(), &v4->sin_addr) != 1) {
        return std::nullopt;
    }
    out.len = sizeof(sockaddr_in);
    return out;
}

} // namespace

bool is_numeric_endpoint(std::string_view address) noexcept {
    return parse_endpoint(address).has_value();
}

std::optional<EndpointScope> endpoint_scope(std::string_view address) noexcept {
    const std::optional<Endpoint> endpoint = parse_endpoint(address);
    if (!endpoint) {
        return std::nullopt;
    }
    // Host order: 0.0.0.0 is unspecified and all of 127.0.0.0/8 is loopback.
    const auto of_v4 = [](std::uint32_t host) {
        if (host == INADDR_ANY) {
            return EndpointScope::Unspecified;
        }
        return (host >> 24U) == 127U ? EndpointScope::Loopback : EndpointScope::Routable;
    };
    if (endpoint->addr.ss_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(&endpoint->addr);
        return of_v4(ntohl(v4->sin_addr.s_addr));
    }
    const auto* v6 = reinterpret_cast<const sockaddr_in6*>(&endpoint->addr);
    const in6_addr& a = v6->sin6_addr;
    if (IN6_IS_ADDR_UNSPECIFIED(&a)) {
        return EndpointScope::Unspecified;
    }
    if (IN6_IS_ADDR_LOOPBACK(&a)) {
        return EndpointScope::Loopback;
    }
    if (IN6_IS_ADDR_V4MAPPED(&a)) {
        // The last four bytes of ::ffff:a.b.c.d are the IPv4 address, in network order.
        std::uint32_t host = 0;
        std::memcpy(&host, &a.s6_addr[12], sizeof host);
        return of_v4(ntohl(host));
    }
    return EndpointScope::Routable;
}

std::expected<os::UniqueFd, int> start_connect(std::string_view address) noexcept {
    const std::optional<Endpoint> endpoint = parse_endpoint(address);
    if (!endpoint) {
        return std::unexpected(EINVAL);
    }
    os::UniqueFd fd{
        ::socket(endpoint->addr.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    if (!fd) {
        return std::unexpected(errno);
    }
    if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&endpoint->addr), endpoint->len) !=
            0 &&
        errno != EINPROGRESS) {
        return std::unexpected(errno);
    }
    return fd;
}

int connect_result(int fd) noexcept {
    int error = 0;
    socklen_t len = sizeof error;
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) != 0) {
        return errno;
    }
    return error;
}

std::expected<os::UniqueFd, int> listen_on(std::string_view address) noexcept {
    const std::optional<Endpoint> endpoint = parse_endpoint(address);
    if (!endpoint) {
        return std::unexpected(EINVAL);
    }
    os::UniqueFd fd{
        ::socket(endpoint->addr.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    if (!fd) {
        return std::unexpected(errno);
    }
    if (auto r = set_int(fd.get(), SOL_SOCKET, SO_REUSEADDR, 1); !r) {
        return std::unexpected(r.error());
    }
    if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&endpoint->addr), endpoint->len) != 0 ||
        ::listen(fd.get(), kBacklog) != 0) {
        return std::unexpected(errno);
    }
    return fd;
}

std::expected<os::UniqueFd, int> listen_tcp(const ListenOptions& options) {
    // Loopback means 127.0.0.1: a socket bound to ::1 takes no IPv4 connections whatever
    // IPV6_V6ONLY says, and local probes dial 127.0.0.1. Otherwise one dual-stack socket
    // serves both families, falling back to IPv4 on hosts without IPv6.
    bool v6 = !options.loopback_only;
    os::UniqueFd fd;
    if (v6) {
        fd = os::UniqueFd{::socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
        v6 = fd || errno != EAFNOSUPPORT;
    }
    if (!v6) {
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
        addr.sin6_addr = in6addr_any;
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

void reset_connection(os::UniqueFd fd) noexcept {
    const linger abortive{.l_onoff = 1, .l_linger = 0};
    // Failing leaves an ordinary close, which refuses the connection all the same.
    static_cast<void>(::setsockopt(fd.get(), SOL_SOCKET, SO_LINGER, &abortive, sizeof abortive));
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

std::expected<void, int> cap_send_buffer(int fd, int bytes) noexcept {
    return set_int(fd, SOL_SOCKET, SO_SNDBUF, bytes);
}

std::expected<void, int> clear_user_timeout(int fd) noexcept {
    return set_int(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, 0);
}

std::expected<os::UniqueFd, int> bind_udp(const SocketAddr& local) {
    const bool v6 = local.family == AddrFamily::V6;
    os::UniqueFd fd{
        ::socket(v6 ? AF_INET6 : AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    if (!fd) {
        return std::unexpected(errno);
    }
    if (v6) {
        if (auto r = set_int(fd.get(), IPPROTO_IPV6, IPV6_V6ONLY, 0); !r) {
            return std::unexpected(r.error());
        }
    }
    sockaddr_storage addr{};
    const auto len = detail::to_sockaddr(local, v6, addr);
    if (!len) {
        return std::unexpected(len.error());
    }
    if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&addr), *len) != 0) {
        return std::unexpected(errno);
    }
    return fd;
}

std::expected<SocketAddr, int> local_addr(int fd) noexcept {
    sockaddr_storage addr{};
    socklen_t len = sizeof addr;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return std::unexpected(errno);
    }
    if (addr.ss_family != AF_INET && addr.ss_family != AF_INET6) {
        return std::unexpected(EAFNOSUPPORT);
    }
    return detail::from_sockaddr(std::as_bytes(std::span(&addr, 1)).first(len));
}

} // namespace net
