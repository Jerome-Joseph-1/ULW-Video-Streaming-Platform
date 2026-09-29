#include "net/reactor_factory.hpp"
#include "net/socket.hpp"
#include "os/system_clock.hpp"

#include "support/reactor_harness.hpp"

#include <sys/socket.h>

#include <cerrno>
#include <gtest/gtest.h>
#include <optional>
#include <string>

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

TEST(NumericEndpoint, StartConnectRefusesANameWithEinval) {
    const auto fd = net::start_connect("localhost:80");
    ASSERT_FALSE(fd);
    EXPECT_EQ(fd.error(), EINVAL);
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

INSTANTIATE_TEST_SUITE_P(Reactors, ConnectTest,
                         ::testing::Values(ReactorKind::IoUring, ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
