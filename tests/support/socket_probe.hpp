#pragma once

#include "support/socket_lookup.hpp"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <cstdint>
#include <optional>

namespace ulw::test {

// The TCP_USER_TIMEOUT, in milliseconds, of this process's own end of a loopback connection:
// the socket bound to `local` and connected to `remote`, which a server under test running in
// the process holds. nullopt when no such socket is open. Only reads its options.
inline std::optional<int> user_timeout_of(std::uint16_t local, std::uint16_t remote) {
    return read_socket(local, remote, [](int fd) -> std::optional<int> {
        int value = 0;
        socklen_t len = sizeof value;
        if (::getsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &value, &len) == 0) {
            return value;
        }
        return std::nullopt;
    });
}

} // namespace ulw::test
