#include "net/reactor_factory.hpp"
#include "net/socket.hpp"
#include "os/system_clock.hpp"

#include "support/reactor_harness.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <vector>

namespace {

using net::ReactorKind;

TEST(NumericEndpoint, TakesAddressesAndRefusesAnythingThatWouldNeedALookup) {
    EXPECT_TRUE(net::is_numeric_endpoint("127.0.0.1:9201"));
    EXPECT_TRUE(net::is_numeric_endpoint("10.1.2.3:1"));
    EXPECT_TRUE(net::is_numeric_endpoint("[::1]:9201"));
    EXPECT_TRUE(net::is_numeric_endpoint("[fd00::5]:65535"));

    EXPECT_FALSE(net::is_numeric_endpoint("localhost:9201"));
    EXPECT_FALSE(net::is_numeric_endpoint("chat-0.chat:9201"));
    EXPECT_FALSE(net::is_numeric_endpoint("127.0.0.1"));
    EXPECT_FALSE(net::is_numeric_endpoint("127.0.0.1:"));
    EXPECT_FALSE(net::is_numeric_endpoint("127.0.0.1:0"));
    EXPECT_FALSE(net::is_numeric_endpoint("127.0.0.1:65536"));
    EXPECT_FALSE(net::is_numeric_endpoint("127.0.0.1:92x"));
    EXPECT_FALSE(net::is_numeric_endpoint("127.0.0.1:+92"));
    // An IPv6 address needs its brackets, or the port cannot be told from the last group.
    EXPECT_FALSE(net::is_numeric_endpoint("::1:9201"));
    EXPECT_FALSE(net::is_numeric_endpoint("[127.0.0.1]:9201"));
    EXPECT_FALSE(net::is_numeric_endpoint("[::1:9201"));
    EXPECT_FALSE(net::is_numeric_endpoint(":9201"));
    EXPECT_FALSE(net::is_numeric_endpoint(""));
}

TEST(NumericEndpoint, TellsAddressesOtherHostsCanDialFromThoseTheyCannot) {
    using net::EndpointScope;
    EXPECT_EQ(net::endpoint_scope("10.42.0.17:9201"), EndpointScope::Routable);
    EXPECT_EQ(net::endpoint_scope("[fd00::5]:9201"), EndpointScope::Routable);
    EXPECT_EQ(net::endpoint_scope("[::ffff:10.1.2.3]:9201"), EndpointScope::Routable);
    EXPECT_EQ(net::endpoint_scope("0.0.0.0:9201"), EndpointScope::Unspecified);
    EXPECT_EQ(net::endpoint_scope("[::]:9201"), EndpointScope::Unspecified);
    EXPECT_EQ(net::endpoint_scope("[::ffff:0.0.0.0]:9201"), EndpointScope::Unspecified);
    EXPECT_EQ(net::endpoint_scope("127.0.0.1:9201"), EndpointScope::Loopback);
    EXPECT_EQ(net::endpoint_scope("127.255.0.9:9201"), EndpointScope::Loopback);
    EXPECT_EQ(net::endpoint_scope("[::1]:9201"), EndpointScope::Loopback);
    EXPECT_EQ(net::endpoint_scope("[::ffff:127.0.0.1]:9201"), EndpointScope::Loopback);
    EXPECT_EQ(net::endpoint_scope("128.0.0.1:9201"), EndpointScope::Routable);
    EXPECT_EQ(net::endpoint_scope("localhost:9201"), std::nullopt);
}

TEST(NumericEndpoint, StartConnectRefusesANameWithEinval) {
    const auto fd = net::start_connect("localhost:80");
    ASSERT_FALSE(fd);
    EXPECT_EQ(fd.error(), EINVAL);
}

TEST(CapSendBuffer, APeerThatStopsReadingHoldsOnlyTheCapInTheKernel) {
    auto listener = net::listen_tcp({.port = 0, .loopback_only = true});
    ASSERT_TRUE(listener);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(*net::local_port(listener->get()));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const os::UniqueFd peer{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): connect() takes any family.
    ASSERT_EQ(::connect(peer.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr), 0);
    const os::UniqueFd conn{
        ::accept4(listener->get(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK)};
    ASSERT_TRUE(conn);

    constexpr int kCap = 64 * 1024;
    ASSERT_TRUE(net::cap_send_buffer(conn.get(), kCap));
    int reported = 0;
    socklen_t len = sizeof reported;
    ASSERT_EQ(::getsockopt(conn.get(), SOL_SOCKET, SO_SNDBUF, &reported, &len), 0);
    EXPECT_EQ(reported, 2 * kCap) << "Linux reports twice what was set";

    // Autotuned, loopback grows the buffer to tcp_wmem's maximum (4 MiB by default) for a
    // peer that never reads. Capped, the kernel holds the doubled cap and whatever the peer's
    // receive window took, a few hundred KiB.
    const std::vector<std::byte> chunk(4096, std::byte{'x'});
    std::size_t held = 0;
    for (;;) {
        const ssize_t n = ::send(conn.get(), chunk.data(), chunk.size(), MSG_NOSIGNAL);
        if (n <= 0) {
            ASSERT_EQ(errno, EAGAIN);
            break;
        }
        held += static_cast<std::size_t>(n);
    }
    EXPECT_LT(held, std::size_t{1} << 20U);
}

class Writable final : public net::IReadyHandler {
public:
    void on_ready(net::Interest /*ready*/) noexcept override { ready = true; }
    bool ready = false;
};

class ConnectTest : public ::testing::TestWithParam<ReactorKind> {
protected:
    void SetUp() override {
        auto r = net::make_reactor(GetParam(), clock_, 1024);
        ASSERT_TRUE(r);
        reactor_ = std::move(*r);
    }

    // Watches `fd` until it turns writable, as a caller on the loop would, and returns the
    // connect's outcome.
    std::optional<int> finish(const os::UniqueFd& fd) {
        Writable writable;
        if (!reactor_->watch(fd.get(), net::Interest::Write, writable)) {
            return std::nullopt;
        }
        const bool ready = ulw::test::pump_until(*reactor_, [&] { return writable.ready; });
        reactor_->unwatch(fd.get());
        if (!ready) {
            return std::nullopt;
        }
        return net::connect_result(fd.get());
    }

    os::SystemClock clock_;
    std::unique_ptr<net::IReactor> reactor_;
};

TEST_P(ConnectTest, ConnectsWithoutBlockingAndTheSocketCarriesBytes) {
    auto listener = net::listen_tcp({.port = 0, .loopback_only = true});
    ASSERT_TRUE(listener);
    const std::uint16_t port = *net::local_port(listener->get());

    auto fd = net::start_connect("127.0.0.1:" + std::to_string(port));
    ASSERT_TRUE(fd);
    EXPECT_EQ(finish(*fd), 0);

    const os::UniqueFd accepted{::accept4(listener->get(), nullptr, nullptr, SOCK_CLOEXEC)};
    ASSERT_TRUE(accepted);
    const std::string hello = "hello";
    ASSERT_EQ(ulw::test::write_some(fd->get(), std::as_bytes(std::span{hello})), hello.size());
    std::vector<std::byte> got;
    ASSERT_TRUE(ulw::test::pump_until(*reactor_, [&] {
        ulw::test::read_some(accepted.get(), got);
        return got.size() == hello.size();
    }));
    EXPECT_EQ(ulw::test::as_text(got), hello);
}

TEST_P(ConnectTest, AClosedPortIsReportedAsRefused) {
    std::uint16_t port = 0;
    {
        auto listener = net::listen_tcp({.port = 0, .loopback_only = true});
        ASSERT_TRUE(listener);
        port = *net::local_port(listener->get());
    }
    auto fd = net::start_connect("127.0.0.1:" + std::to_string(port));
    if (!fd) {
        // Loopback may refuse inside connect() itself.
        EXPECT_EQ(fd.error(), ECONNREFUSED);
        return;
    }
    EXPECT_EQ(finish(*fd), ECONNREFUSED);
}

TEST(ListenOn, TakesConnectionsOnlyAtTheAddressItWasGiven) {
    std::uint16_t port = 0;
    {
        auto probe = net::listen_tcp({.port = 0, .loopback_only = true});
        ASSERT_TRUE(probe);
        port = *net::local_port(probe->get());
    }
    const auto listener = net::listen_on("127.0.0.1:" + std::to_string(port));
    ASSERT_TRUE(listener);
    // The same port on another loopback address belongs to no listener.
    const os::UniqueFd elsewhere{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK + 1);
    // connect() takes every address family through the generic sockaddr header.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    EXPECT_NE(::connect(elsewhere.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr), 0);
    EXPECT_EQ(errno, ECONNREFUSED);
    EXPECT_TRUE(ulw::test::connect_loopback(port));
    EXPECT_EQ(net::listen_on("localhost:9201"), std::unexpected(EINVAL));
}

INSTANTIATE_TEST_SUITE_P(Reactors, ConnectTest,
                         ::testing::Values(ReactorKind::IoUring, ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
