#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace net {

enum class AddrFamily : std::uint8_t { V4, V6 };

// An IP endpoint, port in host order. An IPv4 peer that reaches a dual-stack socket is reported
// as IPv4 rather than as ::ffff:a.b.c.d, so a peer has one identity whichever socket it reaches.
struct SocketAddr {
    AddrFamily family = AddrFamily::V4;
    std::uint16_t port = 0;
    // Tells link-local IPv6 peers on different interfaces apart; zero otherwise.
    std::uint32_t scope_id = 0;
    // IPv4 uses the first four bytes and leaves the rest zero.
    std::array<std::uint8_t, 16> ip{};

    [[nodiscard]] static constexpr SocketAddr v4(std::array<std::uint8_t, 4> a,
                                                 std::uint16_t port) noexcept {
        SocketAddr addr{.family = AddrFamily::V4, .port = port};
        std::ranges::copy(a, addr.ip.begin());
        return addr;
    }

    [[nodiscard]] static constexpr SocketAddr v6(const std::array<std::uint8_t, 16>& a,
                                                 std::uint16_t port,
                                                 std::uint32_t scope_id = 0) noexcept {
        return {.family = AddrFamily::V6, .port = port, .scope_id = scope_id, .ip = a};
    }

    [[nodiscard]] static constexpr SocketAddr loopback(AddrFamily family,
                                                       std::uint16_t port) noexcept {
        if (family == AddrFamily::V4) {
            return v4({127, 0, 0, 1}, port);
        }
        std::array<std::uint8_t, 16> one{};
        one.back() = 1;
        return v6(one, port);
    }

    // As a local address, the IPv6 wildcard takes IPv4 peers as well.
    [[nodiscard]] static constexpr SocketAddr any(AddrFamily family, std::uint16_t port) noexcept {
        return {.family = family, .port = port};
    }

    friend constexpr bool operator==(const SocketAddr&, const SocketAddr&) = default;
};

} // namespace net
