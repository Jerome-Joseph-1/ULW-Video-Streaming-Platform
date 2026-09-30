// A file of its own: glibc's struct tcp_info stops before the fields read here, and
// <linux/tcp.h>, which has them, cannot share a file with <netinet/tcp.h>.
#include "send_window.hpp"

#include <arpa/inet.h>
#include <linux/tcp.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace ulw::test {

namespace {

std::optional<std::uint16_t> port_of(int fd, bool peer) {
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

} // namespace

std::optional<SendWindow> send_window_of(std::uint16_t local, std::uint16_t remote) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it("/proc/self/fd", ec), end; !ec && it != end;
         it.increment(ec)) {
        int fd = -1;
        try {
            fd = std::stoi(it->path().filename().string());
        } catch (const std::exception&) {
            continue;
        }
        if (port_of(fd, false) != local || port_of(fd, true) != remote) {
            continue;
        }
        tcp_info info{};
        socklen_t len = sizeof info;
        if (::getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &len) != 0 ||
            len < offsetof(tcp_info, tcpi_snd_wnd) + sizeof info.tcpi_snd_wnd) {
            return std::nullopt;
        }
        const std::uint64_t sent = info.tcpi_bytes_sent - info.tcpi_bytes_retrans;
        const auto in_flight = static_cast<std::int64_t>(sent - info.tcpi_bytes_acked);
        return SendWindow{.unsent = info.tcpi_notsent_bytes,
                          .in_flight = in_flight,
                          .room = static_cast<std::int64_t>(info.tcpi_snd_wnd) - in_flight,
                          .sent = sent};
    }
    return std::nullopt;
}

} // namespace ulw::test
