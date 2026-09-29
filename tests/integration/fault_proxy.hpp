#pragma once

#include "os/unique_fd.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <list>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace ulw::test {

// A TCP relay in front of the object store that can be told to answer every PUT with a 503,
// for tests of a store that goes away for a while. It passes bytes through untouched, so
// signed requests verify as they do direct: the signature covers the proxy's own host, which
// is the one the store sees.
class FaultProxy {
public:
    // `upstream` is a plain "http://host:port" endpoint.
    explicit FaultProxy(const std::string& upstream) {
        const std::size_t colon = upstream.rfind(':');
        upstream_port_ = static_cast<std::uint16_t>(std::stoul(upstream.substr(colon + 1)));
        listener_ = os::UniqueFd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        sockaddr_in address = loopback(0);
        const int one = 1;
        socklen_t length = sizeof address;
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API's own casts.
        if (::setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0 ||
            ::bind(listener_.get(), reinterpret_cast<const sockaddr*>(&address), sizeof address) !=
                0 ||
            ::listen(listener_.get(), 16) != 0 ||
            ::getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            throw std::runtime_error("fault proxy: cannot listen");
        }
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
        port_ = ntohs(address.sin_port);
        acceptor_ = std::jthread([this](const std::stop_token& stop) { accept_loop(stop); });
    }
    ~FaultProxy() {
        acceptor_.request_stop();
        acceptor_.join();
        const std::lock_guard lock(mutex_);
        for (std::jthread& relay : relays_) {
            relay.request_stop();
        }
        relays_.clear();
    }
    FaultProxy(const FaultProxy&) = delete;
    FaultProxy& operator=(const FaultProxy&) = delete;
    FaultProxy(FaultProxy&&) = delete;
    FaultProxy& operator=(FaultProxy&&) = delete;

    [[nodiscard]] std::string endpoint() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }
    void fail_puts(bool on) { failing_.store(on); }

private:
    static sockaddr_in loopback(std::uint16_t port) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return address;
    }

    void accept_loop(const std::stop_token& stop) {
        while (!stop.stop_requested()) {
            pollfd waiting{.fd = listener_.get(), .events = POLLIN, .revents = 0};
            if (::poll(&waiting, 1, 100) <= 0) {
                continue;
            }
            os::UniqueFd client(::accept4(listener_.get(), nullptr, nullptr, SOCK_CLOEXEC));
            if (!client) {
                continue;
            }
            const std::lock_guard lock(mutex_);
            relays_.emplace_back([this, fd = std::move(client)](const std::stop_token& s) mutable {
                relay(std::move(fd), s);
            });
        }
    }

    void relay(os::UniqueFd client, const std::stop_token& stop) {
        const os::UniqueFd upstream(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        const sockaddr_in address = loopback(upstream_port_);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API's own cast.
        if (::connect(upstream.get(), reinterpret_cast<const sockaddr*>(&address),
                      sizeof address) != 0) {
            return;
        }
        std::array<char, 65536> buffer{};
        while (!stop.stop_requested()) {
            std::array<pollfd, 2> fds{pollfd{.fd = client.get(), .events = POLLIN, .revents = 0},
                                      pollfd{.fd = upstream.get(), .events = POLLIN, .revents = 0}};
            if (::poll(fds.data(), fds.size(), 100) <= 0) {
                continue;
            }
            for (const bool from_client : {true, false}) {
                const pollfd& ready = from_client ? fds.front() : fds.back();
                if (ready.revents == 0) {
                    continue;
                }
                const int from = ready.fd;
                const int to = from_client ? upstream.get() : client.get();
                const ssize_t n = ::read(from, buffer.data(), buffer.size());
                if (n <= 0) {
                    return;
                }
                if (from_client && failing_.load() &&
                    std::string_view(buffer.data(), static_cast<std::size_t>(n))
                        .starts_with("PUT ")) {
                    constexpr std::string_view kRefusal =
                        "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n"
                        "Connection: close\r\n\r\n";
                    [[maybe_unused]] const ssize_t sent =
                        ::write(client.get(), kRefusal.data(), kRefusal.size());
                    return;
                }
                if (::write(to, buffer.data(), static_cast<std::size_t>(n)) != n) {
                    return;
                }
            }
        }
    }

    os::UniqueFd listener_;
    std::uint16_t port_ = 0;
    std::uint16_t upstream_port_ = 0;
    std::atomic<bool> failing_{false};
    std::mutex mutex_;
    std::list<std::jthread> relays_;
    std::jthread acceptor_;
};

} // namespace ulw::test
