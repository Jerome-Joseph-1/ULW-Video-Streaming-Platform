#pragma once

#include "os/unique_fd.hpp"

#include <cstdint>
#include <expected>

namespace net {

struct ListenOptions {
    std::uint16_t port = 0;
    bool loopback_only = false;
    // One listener per shard; the kernel spreads connections across them.
    bool reuse_port = false;
};

// Dual-stack (IPv6 with V6ONLY off) where the host has IPv6, IPv4 otherwise. Nonblocking,
// close-on-exec, backlog 1024.
[[nodiscard]] std::expected<os::UniqueFd, int> listen_tcp(const ListenOptions& options);
[[nodiscard]] std::expected<std::uint16_t, int> local_port(int fd) noexcept;

// TCP_NODELAY, keepalive 60/10/3, TCP_USER_TIMEOUT 20 s. Buffer sizes are deliberately left
// alone: setting SO_RCVBUF or SO_SNDBUF switches off the kernel's autotuning.
[[nodiscard]] std::expected<void, int> tune_connection(int fd) noexcept;

} // namespace net
