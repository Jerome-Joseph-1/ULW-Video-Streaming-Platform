#include "net/socket.hpp"
#include "net/socket_addr.hpp"

#include "sockaddr.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <cerrno>
#include <cstring>
#include <gtest/gtest.h>
#include <span>

namespace {

using net::AddrFamily;
using net::SocketAddr;
using net::detail::from_sockaddr;
using net::detail::to_sockaddr;

template <class T> std::span<const std::byte> bytes_of(const T& raw) {
    return std::as_bytes(std::span(&raw, 1));
}

sockaddr_in6 v6_raw(const char* text, std::uint16_t port, std::uint32_t scope = 0) {
    sockaddr_in6 raw{};
    raw.sin6_family = AF_INET6;
    raw.sin6_port = htons(port);
    raw.sin6_scope_id = scope;
    EXPECT_EQ(::inet_pton(AF_INET6, text, &raw.sin6_addr), 1);
    return raw;
}

TEST(SockaddrTest, Ipv4RoundTripsThroughAnIpv4Socket) {
    const SocketAddr addr = SocketAddr::v4({192, 0, 2, 7}, 5004);
    sockaddr_storage raw{};
    const auto len = to_sockaddr(addr, false, raw);
    ASSERT_TRUE(len);
    EXPECT_EQ(*len, sizeof(sockaddr_in));
    sockaddr_in in{};
    std::memcpy(&in, &raw, sizeof in);
    EXPECT_EQ(ntohs(in.sin_port), 5004);
    EXPECT_EQ(ntohl(in.sin_addr.s_addr), 0xC0000207U);
    EXPECT_EQ(from_sockaddr(bytes_of(raw).first(*len)), addr);
}

TEST(SockaddrTest, V4MappedSourceIsReportedAsIpv4) {
    const auto raw = v6_raw("::ffff:192.0.2.7", 5004);
    EXPECT_EQ(from_sockaddr(bytes_of(raw)), SocketAddr::v4({192, 0, 2, 7}, 5004));
}

TEST(SockaddrTest, Ipv4DestinationOnAnIpv6SocketIsWrittenV4Mapped) {
    sockaddr_storage raw{};
    const auto len = to_sockaddr(SocketAddr::v4({192, 0, 2, 7}, 5004), true, raw);
    ASSERT_TRUE(len);
    EXPECT_EQ(*len, sizeof(sockaddr_in6));
    const auto expected = v6_raw("::ffff:192.0.2.7", 5004);
    sockaddr_in6 in6{};
    std::memcpy(&in6, &raw, sizeof in6);
    EXPECT_EQ(in6.sin6_family, AF_INET6);
    EXPECT_EQ(in6.sin6_port, expected.sin6_port);
    EXPECT_EQ(std::memcmp(&in6.sin6_addr, &expected.sin6_addr, sizeof expected.sin6_addr), 0);
}

TEST(SockaddrTest, Ipv6KeepsItsAddressAndScope) {
    const auto raw = v6_raw("fe80::1:2", 443, 3);
    const SocketAddr addr = from_sockaddr(bytes_of(raw));
    EXPECT_EQ(addr.family, AddrFamily::V6);
    EXPECT_EQ(addr.port, 443);
    EXPECT_EQ(addr.scope_id, 3U);
    EXPECT_EQ(addr.ip[0], 0xFE);
    EXPECT_EQ(addr.ip[15], 0x02);

    sockaddr_storage back{};
    ASSERT_TRUE(to_sockaddr(addr, true, back));
    EXPECT_EQ(from_sockaddr(bytes_of(back)), addr);
}

TEST(SockaddrTest, Ipv6DestinationOnAnIpv4SocketIsRefused) {
    sockaddr_storage raw{};
    EXPECT_EQ(to_sockaddr(SocketAddr::loopback(AddrFamily::V6, 1), false, raw).error_or(0),
              EAFNOSUPPORT);
}

TEST(SockaddrTest, BoundUdpSocketReportsItsAddress) {
    auto fd = net::bind_udp(SocketAddr::loopback(AddrFamily::V4, 0));
    ASSERT_TRUE(fd);
    const auto local = net::local_addr(fd->get());
    ASSERT_TRUE(local);
    EXPECT_EQ(*local, SocketAddr::loopback(AddrFamily::V4, local->port));
    EXPECT_NE(local->port, 0);
}

} // namespace
