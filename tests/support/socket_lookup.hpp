#pragma once

// Neither <netinet/tcp.h> nor <linux/tcp.h>, so a file that needs either can use it.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <exception>
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

// Calls `read` with each socket of this process bound to `local` and connected to `remote`,
// and returns the first answer it gives; nullopt when it gives none. `read` returns an
// optional and only reads the socket: a server under test in the process owns it.
template <class Read>
auto read_socket(std::uint16_t local, std::uint16_t remote, Read read) -> decltype(read(0)) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it("/proc/self/fd", ec), end; !ec && it != end;
         it.increment(ec)) {
        int fd = -1;
        try {
            fd = std::stoi(it->path().filename().string());
        } catch (const std::exception&) {
            continue;
        }
        if (tcp_port(fd, false) != local || tcp_port(fd, true) != remote) {
            continue;
        }
        if (auto answer = read(fd)) {
            return answer;
        }
    }
    return std::nullopt;
}

} // namespace ulw::test
