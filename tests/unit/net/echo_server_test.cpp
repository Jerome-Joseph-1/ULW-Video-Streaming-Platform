#include "net/reactor_factory.hpp"
#include "net/socket.hpp"

#include "echo_server.hpp"
#include "support/fake_clock.hpp"
#include "support/reactor_harness.hpp"

#include <sys/wait.h>

#include <cstring>
#include <gtest/gtest.h>
#include <unistd.h>

namespace {

using core::Millis;
using net::ReactorKind;
using ulw::test::connect_loopback;
using ulw::test::pump_until;
using ulw::test::read_some;
using ulw::test::write_some;

class EchoServerTest : public ::testing::TestWithParam<ReactorKind> {
protected:
    void SetUp() override {
        auto r = net::make_reactor(GetParam(), clock, 4096);
        ASSERT_TRUE(r);
        reactor = std::move(*r);
        restart({});
    }

    void restart(const ulw::test::EchoOptions& options) {
        if (server) {
            reactor->stop_listening();
            server.reset();
        }
        server = std::make_unique<ulw::test::EchoServer>(*reactor, options);
        auto listener = net::listen_tcp({.port = 0, .loopback_only = true});
        ASSERT_TRUE(listener);
        port = *net::local_port(listener->get());
        ASSERT_TRUE(reactor->listen(std::move(*listener), *server));
    }

    void TearDown() override { server.reset(); }

    // Real I/O, fake time: the loop is pumped without waiting, and the clock only moves when
    // a test says so.
    template <class Pred> bool settle(Pred pred) {
        return pump_until(*reactor, [&] {
            server->reap();
            return pred();
        });
    }

    void advance(Millis d) {
        clock.advance(d);
        reactor->run_once(Millis{0});
        server->reap();
    }

    static bool closed_by_peer(int fd) {
        std::byte b{};
        return ::recv(fd, &b, 1, MSG_DONTWAIT) == 0;
    }

    ulw::test::FakeClock clock;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<ulw::test::EchoServer> server;
    std::uint16_t port = 0;
};

TEST_P(EchoServerTest, IdleConnectionClosesAfterTenSeconds) {
    auto client = connect_loopback(port);
    ASSERT_TRUE(settle([&] { return server->connections() == 1; }));
    advance(Millis{9'000});
    EXPECT_EQ(server->connections(), 1U);
    EXPECT_FALSE(closed_by_peer(client.get()));
    advance(Millis{1'200});
    ASSERT_TRUE(settle([&] { return server->connections() == 0; }));
    EXPECT_TRUE(closed_by_peer(client.get()));
}

TEST_P(EchoServerTest, ActivityPushesTheIdleDeadlineBack) {
    auto client = connect_loopback(port);
    ASSERT_TRUE(settle([&] { return server->connections() == 1; }));
    advance(Millis{8'000});
    const std::string_view msg = "still here";
    ASSERT_EQ(write_some(client.get(), std::as_bytes(std::span(msg))), msg.size());
    std::vector<std::byte> echoed;
    ASSERT_TRUE(settle([&] {
        read_some(client.get(), echoed);
        return echoed.size() == msg.size();
    }));
    advance(Millis{8'000});
    EXPECT_EQ(server->connections(), 1U);
    advance(Millis{2'200});
    EXPECT_TRUE(settle([&] { return server->connections() == 0; }));
}

TEST_P(EchoServerTest, DrainClosesIdleConnectionsAndRefusesNewOnes) {
    std::vector<os::UniqueFd> clients;
    for (int i = 0; i < 5; ++i) {
        clients.push_back(connect_loopback(port));
    }
    ASSERT_TRUE(settle([&] { return server->connections() == 5; }));
    server->begin_drain();
    ASSERT_TRUE(settle([&] { return server->finished(); }));
    EXPECT_FALSE(connect_loopback(port));
}

TEST_P(EchoServerTest, DrainDeadlineClosesPeersThatNeverReadTheirEcho) {
    // The idle timeout would otherwise close the stuck peer first.
    restart({.idle_timeout = Millis{600'000}});
    auto client = connect_loopback(port);
    ASSERT_TRUE(settle([&] { return server->connections() == 1; }));
    // Write until both directions are saturated: the kernel buffers are full and the server
    // has stopped reading with its own echo queued, which is the state a drain must bound.
    const auto data = ulw::test::pattern(ulw::test::kMiB);
    int stalled = 0;
    ASSERT_TRUE(settle([&] {
        stalled = write_some(client.get(), data) == 0 ? stalled + 1 : 0;
        return stalled > 200;
    }));
    server->begin_drain();
    advance(Millis{29'000});
    ulw::test::pump_for(*reactor, std::chrono::milliseconds(50));
    server->reap();
    EXPECT_EQ(server->connections(), 1U);
    advance(Millis{1'200});
    EXPECT_TRUE(settle([&] { return server->finished(); }));
}

INSTANTIATE_TEST_SUITE_P(Reactors, EchoServerTest,
                         ::testing::Values(ReactorKind::IoUring, ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
