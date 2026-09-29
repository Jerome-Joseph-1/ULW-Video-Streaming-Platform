#pragma once

#include "os/unique_fd.hpp"

#include <sys/socket.h>
#include <sys/un.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <poll.h>
#include <string>

namespace ulw::test {

// Stands in for the service manager: a datagram socket bound where NOTIFY_SOCKET points, a
// path or (with a leading NUL) an abstract name.
class FakeNotifySocket {
public:
    explicit FakeNotifySocket(const std::string& address) {
        fd_.reset(::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::memcpy(static_cast<void*>(addr.sun_path), address.data(), address.size());
        const auto len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + address.size());
        // bind() takes every address family through the generic header.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        bound_ = ::bind(fd_.get(), reinterpret_cast<const sockaddr*>(&addr), len) == 0;
    }

    [[nodiscard]] bool bound() const noexcept { return bound_; }

    // The next datagram, or empty when none arrives within `limit`.
    [[nodiscard]] std::string
    receive(std::chrono::milliseconds limit = std::chrono::milliseconds(0)) const {
        pollfd p{.fd = fd_.get(), .events = POLLIN, .revents = 0};
        if (::poll(&p, 1, static_cast<int>(limit.count())) <= 0) {
            return {};
        }
        std::array<char, 512> buf{};
        const ssize_t n = ::recv(fd_.get(), buf.data(), buf.size(), 0);
        return n <= 0 ? std::string{} : std::string(buf.data(), static_cast<std::size_t>(n));
    }

    // Reads datagrams until `message` arrives or `limit` passes.
    [[nodiscard]] bool wait_for(const std::string& message, std::chrono::milliseconds limit) const {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (receive(left) == message) {
                return true;
            }
        }
        return false;
    }

private:
    os::UniqueFd fd_;
    bool bound_ = false;
};

} // namespace ulw::test
