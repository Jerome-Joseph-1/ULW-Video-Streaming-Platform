#pragma once

#include "net/socket_addr.hpp"
#include "os/unique_fd.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

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
// Fixes the kernel's send buffer at `bytes` (Linux doubles it for its own bookkeeping), for a
// server whose connections carry little and must not each hold megabytes for a peer that
// stopped reading. Autotuning is off for that socket from then on.
[[nodiscard]] std::expected<void, int> cap_send_buffer(int fd, int bytes) noexcept;

// A numeric "ipv4:port" or "[ipv6]:port": what start_connect accepts. Names are refused, since
// resolving one would block the loop.
[[nodiscard]] bool is_numeric_endpoint(std::string_view address) noexcept;

// Who a numeric endpoint's address can be reached by. Unspecified (0.0.0.0, ::) names no host
// at all, and loopback only the host itself; neither is an address another host can dial.
enum class EndpointScope : std::uint8_t { Unspecified, Loopback, Routable };

// nullopt for anything is_numeric_endpoint refuses. IPv4 addresses mapped into IPv6 count as
// the IPv4 address they carry.
[[nodiscard]] std::optional<EndpointScope> endpoint_scope(std::string_view address) noexcept;

// Starts a nonblocking, close-on-exec connect to a numeric endpoint. It has finished once the
// socket turns writable (watch it for Write), and connect_result() then says how.
[[nodiscard]] std::expected<os::UniqueFd, int> start_connect(std::string_view address) noexcept;
// Listens on exactly one numeric endpoint, for a port that must not be reachable on every
// interface. Nonblocking, close-on-exec, backlog 1024.
[[nodiscard]] std::expected<os::UniqueFd, int> listen_on(std::string_view address) noexcept;
// 0 once a started connect has succeeded, otherwise the errno it failed with.
[[nodiscard]] int connect_result(int fd) noexcept;

// A nonblocking, close-on-exec UDP socket bound to `local`. IPv6 sockets are dual-stack whatever
// net.ipv6.bindv6only says, so binding the IPv6 wildcard takes IPv4 peers too.
[[nodiscard]] std::expected<os::UniqueFd, int> bind_udp(const SocketAddr& local);
[[nodiscard]] std::expected<SocketAddr, int> local_addr(int fd) noexcept;

} // namespace net
