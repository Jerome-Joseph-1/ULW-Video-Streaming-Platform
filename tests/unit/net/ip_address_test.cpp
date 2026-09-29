#include "net/ip_address.hpp"
#include "net/socket.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <gtest/gtest.h>
#include <unistd.h>

namespace {

net::IpAddress ip(std::string_view text) {
    const auto a = net::IpAddress::parse(text);
    EXPECT_TRUE(a) << text;
    return a.value_or(net::IpAddress{});
}

TEST(IpAddress, ParsesNumericAddressesOnly) {
    EXPECT_TRUE(net::IpAddress::parse("203.0.113.7"));
    EXPECT_TRUE(net::IpAddress::parse("2001:db8::1"));
    EXPECT_TRUE(net::IpAddress::parse("::1"));

    EXPECT_FALSE(net::IpAddress::parse(""));
    EXPECT_FALSE(net::IpAddress::parse("localhost"));
    EXPECT_FALSE(net::IpAddress::parse("203.0.113.7:443"));
    EXPECT_FALSE(net::IpAddress::parse("[2001:db8::1]"));
    EXPECT_FALSE(net::IpAddress::parse(" 203.0.113.7"));
    EXPECT_FALSE(net::IpAddress::parse("203.0.113.256"));
    EXPECT_FALSE(net::IpAddress::parse(std::string(64, '1')));
}

TEST(IpAddress, AnIpv4AddressEqualsItsMappedIpv6Form) {
    EXPECT_EQ(ip("198.51.100.20"), ip("::ffff:198.51.100.20"));
    EXPECT_TRUE(ip("198.51.100.20").is_v4());
    EXPECT_FALSE(ip("2001:db8::1").is_v4());
    EXPECT_NE(ip("198.51.100.20"), ip("198.51.100.21"));
}

TEST(IpAddress, PrintsInTheFormItsFamilyIsWrittenIn) {
    EXPECT_EQ(ip("198.51.100.20").to_string(), "198.51.100.20");
    EXPECT_EQ(ip("::ffff:198.51.100.20").to_string(), "198.51.100.20");
    EXPECT_EQ(ip("2001:DB8:0:0::1").to_string(), "2001:db8::1");
}

TEST(IpAddress, PrefixClearsEveryBitPastIt) {
    EXPECT_EQ(ip("2001:db8:1:2:3:4:5:6").prefix(64), ip("2001:db8:1:2::"));
    EXPECT_EQ(ip("2001:db8:1:2:3:4:5:6").prefix(60), ip("2001:db8:1::"));
    EXPECT_EQ(ip("2001:db8::1").prefix(128), ip("2001:db8::1"));
    EXPECT_EQ(ip("2001:db8::1").prefix(0), ip("::"));
}

TEST(IpNetwork, ContainsExactlyTheAddressesUnderItsPrefix) {
    const auto pods = net::IpNetwork::parse("10.42.0.0/16");
    ASSERT_TRUE(pods);
    EXPECT_TRUE(pods->contains(ip("10.42.0.1")));
    EXPECT_TRUE(pods->contains(ip("10.42.255.255")));
    EXPECT_FALSE(pods->contains(ip("10.43.0.1")));
    EXPECT_FALSE(pods->contains(ip("10.41.255.255")));
    // The same 32 bits in an IPv6 address that is not IPv4-mapped are another address.
    EXPECT_FALSE(pods->contains(ip("::a2a:1")));

    const auto v6 = net::IpNetwork::parse("fd00::/8");
    ASSERT_TRUE(v6);
    EXPECT_TRUE(v6->contains(ip("fd12:3456::1")));
    EXPECT_FALSE(v6->contains(ip("fe80::1")));
    EXPECT_FALSE(v6->contains(ip("10.42.0.1")));
}

TEST(IpNetwork, ReportsItsPrefixAsWritten) {
    EXPECT_EQ(net::IpNetwork::parse("10.42.0.0/16")->prefix_length(), 16U);
    EXPECT_EQ(net::IpNetwork::parse("192.0.2.9")->prefix_length(), 32U);
    EXPECT_EQ(net::IpNetwork::parse("fd00::/8")->prefix_length(), 8U);
}

TEST(IpNetwork, ABareAddressIsABlockOfOne) {
    const auto one = net::IpNetwork::parse("192.0.2.9");
    ASSERT_TRUE(one);
    EXPECT_TRUE(one->contains(ip("192.0.2.9")));
    EXPECT_FALSE(one->contains(ip("192.0.2.8")));
    const auto all = net::IpNetwork::parse("0.0.0.0/0");
    ASSERT_TRUE(all);
    EXPECT_TRUE(all->contains(ip("255.255.255.255")));
    EXPECT_FALSE(all->contains(ip("2001:db8::1")));
}

TEST(IpNetwork, RefusesBlocksThatDoNotSayWhatTheyMean) {
    // Host bits set past the prefix.
    EXPECT_FALSE(net::IpNetwork::parse("10.42.1.0/16"));
    EXPECT_FALSE(net::IpNetwork::parse("fd00::1/8"));
    EXPECT_FALSE(net::IpNetwork::parse("10.0.0.0/33"));
    EXPECT_FALSE(net::IpNetwork::parse("fd00::/129"));
    EXPECT_FALSE(net::IpNetwork::parse("10.0.0.0/"));
    EXPECT_FALSE(net::IpNetwork::parse("10.0.0.0/+8"));
    EXPECT_FALSE(net::IpNetwork::parse("10.0.0.0/8x"));
    EXPECT_FALSE(net::IpNetwork::parse("/8"));
    EXPECT_FALSE(net::IpNetwork::parse("pods/16"));
}

// A listener on 127.0.0.1 and one accepted connection from a blocking client.
struct Pair {
    os::UniqueFd listener;
    os::UniqueFd client;
    os::UniqueFd accepted;
};

Pair connected_pair() {
    Pair p;
    auto listener = net::listen_tcp({.port = 0, .loopback_only = true});
    EXPECT_TRUE(listener);
    p.listener = std::move(*listener);
    const auto port = net::local_port(p.listener.get());
    EXPECT_TRUE(port);
    p.client = os::UniqueFd{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port.value_or(0));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // connect() takes every address family through the generic sockaddr header.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    EXPECT_EQ(::connect(p.client.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr), 0);
    // The handshake completed in connect, so the connection is already queued.
    p.accepted = os::UniqueFd{::accept4(p.listener.get(), nullptr, nullptr, SOCK_CLOEXEC)};
    EXPECT_TRUE(p.accepted);
    return p;
}

TEST(PeerAddress, NamesTheConnectingHost) {
    const Pair p = connected_pair();
    const auto peer = net::peer_address(p.accepted.get());
    ASSERT_TRUE(peer);
    EXPECT_EQ(*peer, ip("127.0.0.1"));
    EXPECT_FALSE(net::peer_address(p.listener.get()));
}

TEST(ResetConnection, ThePeerSeesAResetNotAnOrderlyClose) {
    Pair p = connected_pair();
    net::reset_connection(std::move(p.accepted));
    std::array<char, 16> buf{};
    const ssize_t n = ::recv(p.client.get(), buf.data(), buf.size(), 0);
    EXPECT_EQ(n, -1);
    EXPECT_EQ(errno, ECONNRESET);
}

} // namespace
