#pragma once

#include "net/socket_addr.hpp"

#include <sys/socket.h>

#include <cstddef>
#include <expected>
#include <span>

namespace net::detail {

// `name` is the address as the kernel wrote it (recvfrom, recvmsg, getsockname). v4-mapped IPv6
// addresses come back as IPv4.
[[nodiscard]] SocketAddr from_sockaddr(std::span<const std::byte> name) noexcept;

// For a socket of the given family. An IPv4 destination on an IPv6 socket is written v4-mapped;
// an IPv6 destination on an IPv4 socket is EAFNOSUPPORT.
[[nodiscard]] std::expected<socklen_t, int> to_sockaddr(const SocketAddr& addr, bool v6_socket,
                                                        sockaddr_storage& out) noexcept;

} // namespace net::detail
