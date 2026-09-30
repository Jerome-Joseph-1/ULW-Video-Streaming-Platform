#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace ulw::test {

// The TCP port a socket is bound or connected to at one end; nullopt for anything else.
inline std::optional<std::uint16_t> tcp_port(int fd, bool peer) {
    sockaddr_storage addr{};
    socklen_t len = sizeof addr;
    // Both calls take every address family through the generic sockaddr header.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto* generic = reinterpret_cast<sockaddr*>(&addr);
    if ((peer ? ::getpeername(fd, generic, &len) : ::getsockname(fd, generic, &len)) != 0) {
        return std::nullopt;
    }
    if (addr.ss_family == AF_INET) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        return ntohs(reinterpret_cast<const sockaddr_in*>(&addr)->sin_port);
    }
    if (addr.ss_family == AF_INET6) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&addr)->sin6_port);
    }
    return std::nullopt;
}

// The TCP_USER_TIMEOUT, in milliseconds, of this process's own end of a loopback connection:
// the socket bound to `local` and connected to `remote`, which a server under test running in
// the process holds. nullopt when no such socket is open. Only reads its options.
inline std::optional<int> user_timeout_of(std::uint16_t local, std::uint16_t remote) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd", ec)) {
        int fd = -1;
        try {
            fd = std::stoi(entry.path().filename().string());
        } catch (const std::exception&) {
            continue;
        }
        if (tcp_port(fd, false) != local || tcp_port(fd, true) != remote) {
            continue;
        }
        int value = 0;
        socklen_t len = sizeof value;
        if (::getsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &value, &len) == 0) {
            return value;
        }
    }
    return std::nullopt;
}

} // namespace ulw::test
