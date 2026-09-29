#include "sockaddr.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace net::detail {

namespace {

constexpr std::array<std::uint8_t, 12> kV4MappedPrefix{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};

} // namespace

SocketAddr from_sockaddr(std::span<const std::byte> name) noexcept {
    sockaddr_storage raw{};
    std::memcpy(&raw, name.data(), std::min(name.size(), sizeof raw));
    SocketAddr addr;
    if (raw.ss_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(&raw);
        addr.port = ntohs(in->sin_port);
        std::memcpy(addr.ip.data(), &in->sin_addr, sizeof in->sin_addr);
        return addr;
    }
    if (raw.ss_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&raw);
        addr.port = ntohs(in6->sin6_port);
        std::array<std::uint8_t, 16> bytes{};
        std::memcpy(bytes.data(), &in6->sin6_addr, bytes.size());
        if (std::equal(kV4MappedPrefix.begin(), kV4MappedPrefix.end(), bytes.begin())) {
            std::copy_n(bytes.begin() + kV4MappedPrefix.size(), 4, addr.ip.begin());
            return addr;
        }
        addr.family = AddrFamily::V6;
        addr.ip = bytes;
        addr.scope_id = in6->sin6_scope_id;
    }
    return addr;
}

std::expected<socklen_t, int> to_sockaddr(const SocketAddr& addr, bool v6_socket,
                                          sockaddr_storage& out) noexcept {
    out = {};
    if (!v6_socket) {
        if (addr.family != AddrFamily::V4) {
            return std::unexpected(EAFNOSUPPORT);
        }
        auto* in = reinterpret_cast<sockaddr_in*>(&out);
        in->sin_family = AF_INET;
        in->sin_port = htons(addr.port);
        std::memcpy(&in->sin_addr, addr.ip.data(), sizeof in->sin_addr);
        return static_cast<socklen_t>(sizeof(sockaddr_in));
    }
    auto* in6 = reinterpret_cast<sockaddr_in6*>(&out);
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons(addr.port);
    if (addr.family == AddrFamily::V4) {
        std::array<std::uint8_t, 16> mapped{};
        std::ranges::copy(kV4MappedPrefix, mapped.begin());
        std::copy_n(addr.ip.begin(), 4, mapped.begin() + kV4MappedPrefix.size());
        std::memcpy(&in6->sin6_addr, mapped.data(), mapped.size());
    } else {
        std::memcpy(&in6->sin6_addr, addr.ip.data(), addr.ip.size());
        in6->sin6_scope_id = addr.scope_id;
    }
    return static_cast<socklen_t>(sizeof(sockaddr_in6));
}

} // namespace net::detail
