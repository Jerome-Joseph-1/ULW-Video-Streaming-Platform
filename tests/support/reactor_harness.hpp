#pragma once

#include "net/reactor.hpp"
#include "net/reactor_factory.hpp"
#include "os/unique_fd.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <span>
#include <string>
#include <vector>

namespace ulw::test {

inline constexpr std::size_t kMiB = std::size_t{1024} * 1024;
inline constexpr std::size_t kKiB = 1024;

// Drives the loop until `pred` holds or `limit` of real time passes. The wait inside
// run_once is the only blocking, so a test never sleeps.
template <class Pred>
bool pump_until(net::IReactor& reactor, Pred pred,
                std::chrono::milliseconds limit = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        reactor.run_once(core::Millis{5});
    }
    return true;
}

inline void pump_for(net::IReactor& reactor, std::chrono::milliseconds span) {
    const auto deadline = std::chrono::steady_clock::now() + span;
    while (std::chrono::steady_clock::now() < deadline) {
        reactor.run_once(core::Millis{5});
    }
}

inline os::UniqueFd connect_loopback(std::uint16_t port) {
    os::UniqueFd fd{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (!fd) {
        return {};
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        return {};
    }
    ::fcntl(fd.get(), F_SETFL, ::fcntl(fd.get(), F_GETFL) | O_NONBLOCK);
    return fd;
}

// Nonblocking; returns bytes written (0 when the socket buffer is full).
inline std::size_t write_some(int fd, std::span<const std::byte> bytes) {
    const ssize_t n = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    return n > 0 ? static_cast<std::size_t>(n) : 0;
}

inline std::size_t read_some(int fd, std::vector<std::byte>& into) {
    std::array<std::byte, 65536> buf{};
    std::size_t total = 0;
    for (;;) {
        const ssize_t n = ::recv(fd, buf.data(), buf.size(), MSG_DONTWAIT);
        if (n <= 0) {
            return total;
        }
        into.insert(into.end(), buf.begin(), buf.begin() + n);
        total += static_cast<std::size_t>(n);
    }
}

// Parameter-name generator for suites instantiated over both reactors.
inline std::string reactor_name(const ::testing::TestParamInfo<net::ReactorKind>& param) {
    return param.param == net::ReactorKind::IoUring ? "IoUring" : "Epoll";
}

inline std::vector<std::byte> pattern(std::size_t n, std::size_t seed = 0) {
    std::vector<std::byte> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = static_cast<std::byte>(((i + seed) * 31 + 7) % 251);
    }
    return v;
}

} // namespace ulw::test
