// A file of its own: glibc's struct tcp_info stops before the fields read here, and
// <linux/tcp.h>, which has them, cannot share a file with <netinet/tcp.h>.
#include "send_window.hpp"

#include "support/socket_lookup.hpp"

#include <linux/tcp.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace ulw::test {

std::optional<SendWindow> send_window_of(std::uint16_t local, std::uint16_t remote) {
    return read_socket(local, remote, [](int fd) -> std::optional<SendWindow> {
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
    });
}

} // namespace ulw::test
