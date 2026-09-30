#pragma once

#include "os/unique_fd.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <random>

namespace ulw::test {

namespace detail {

inline bool bindable(std::uint32_t address, std::uint16_t port) {
    const os::UniqueFd fd{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (!fd) {
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(address);
    addr.sin_port = htons(port);
    // bind() takes every address family through the generic header. SO_REUSEADDR stays off: a
    // port with a lingering connection is not one a child should be handed.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return ::bind(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0;
}

struct PortWindow {
    std::uint32_t first = 0;
    std::uint32_t end = 0; // one past the last
};

// Outgoing connections (Postgres clients, the node channel, HTTP probes) take their local port
// from net.ipv4.ip_local_port_range, so a port picked there by bind(0) can be someone's source
// port by the time a child binds it. Ports below the range's lower bound never are.
inline PortWindow port_window() {
    std::uint32_t low = 32768;
    std::uint32_t high = 60999;
    if (std::ifstream range("/proc/sys/net/ipv4/ip_local_port_range"); range) {
        range >> low >> high;
    }
    // A margin under the range, and above the registered ports many services use.
    constexpr std::uint32_t kFloor = 20000;
    constexpr std::uint32_t kMargin = 1000;
    if (low > kFloor + kMargin) {
        return {.first = kFloor, .end = low - kMargin};
    }
    // A host that widened the range down to the well-known ports: use what is above it.
    return {.first = std::min<std::uint32_t>(high + 1, 65535), .end = 65535};
}

} // namespace detail

// A loopback and wildcard port nothing holds right now and no outgoing connection can take as its
// source port. Consecutive calls in a process never repeat a port, so several reservations made
// before any child binds are distinct; another process racing for the same port is caught by
// start_until_listening (child_process.hpp). 0 when none is found.
inline std::uint16_t reserve_port() {
    static const detail::PortWindow window = detail::port_window();
    static std::atomic<std::uint32_t> cursor = [] {
        const std::uint32_t span = std::max<std::uint32_t>(window.end - window.first, 1);
        // Seeded per process so parallel test binaries start in different places.
        return std::random_device{}() % span;
    }();
    const std::uint32_t span = window.end - window.first;
    for (std::uint32_t tried = 0; tried < span; ++tried) {
        const auto port = static_cast<std::uint16_t>(window.first + cursor.fetch_add(1) % span);
        if (detail::bindable(INADDR_LOOPBACK, port) && detail::bindable(INADDR_ANY, port)) {
            return port;
        }
    }
    return 0;
}

} // namespace ulw::test
