// Apart from socket.cpp: glibc's struct tcp_info stops before the fields read here, and
// <linux/tcp.h>, which has them, cannot share a file with <netinet/tcp.h>.
#include "net/socket.hpp"

#include <linux/tcp.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstddef>

namespace net {

std::expected<SendProgress, int> send_progress(int fd) noexcept {
    tcp_info info{};
    socklen_t len = sizeof info;
    if (::getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &len) != 0) {
        return std::unexpected(errno);
    }
    // Kernels before 4.6 fill in less.
    if (len < offsetof(tcp_info, tcpi_notsent_bytes) + sizeof info.tcpi_notsent_bytes) {
        return std::unexpected(ENOPROTOOPT);
    }
    return SendProgress{.acked = info.tcpi_bytes_acked,
                        .waiting = info.tcpi_unacked != 0 || info.tcpi_notsent_bytes != 0,
                        .unsent = info.tcpi_notsent_bytes != 0};
}

} // namespace net
