#pragma once

#include "os/unique_fd.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>

namespace ulw::test {

// A loopback port nothing listens on right now. A server started a moment later takes it;
// nothing else on a test host binds loopback ports in between.
inline std::uint16_t free_port() {
    const os::UniqueFd fd{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (!fd) {
        return 0;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof addr;
    // bind() and getsockname() take every address family through the generic header.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0 ||
        ::getsockname(fd.get(), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return 0;
    }
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    return ntohs(addr.sin_port);
}

} // namespace ulw::test
