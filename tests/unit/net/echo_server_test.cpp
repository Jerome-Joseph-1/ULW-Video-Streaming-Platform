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

    // Writes until both directions are saturated: the kernel buffers are full and the server
    // has stopped reading with its own echo queued, which is the state a drain must bound.
    bool saturate(int client) {
        const auto data = ulw::test::pattern(ulw::test::kMiB);
        int stalled = 0;
        return settle([&] {
            stalled = write_some(client, data) == 0 ? stalled + 1 : 0;
            return stalled > 200;
        });
    }

    static bool closed_by_peer(int fd) {
        std::byte b{};
        return ::recv(fd, &b, 1, MSG_DONTWAIT) == 0;
    }

    // The child's half of PortIsFreeTheMomentADrainedServerProcessExits.
    int run_child_server() {
        auto r = net::make_reactor(GetParam(), clock, 4096);
        auto listener = net::listen_tcp({.port = port, .loopback_only = true});
        if (!r || !listener) {
            return 3;
        }
        ulw::test::EchoServer child(**r, {});
        if (!(*r)->listen(std::move(*listener), child)) {
            return 4;
        }
        std::vector<os::UniqueFd> clients;
        for (int i = 0; i < 300; ++i) {
            clients.push_back(connect_loopback(port));
        }
        pump_until(**r, [&] {
            child.reap();
            return child.connections() == clients.size();
        });
        for (auto& c : clients) {
            write_some(c.get(), std::as_bytes(std::span(std::string_view("ping"))));
        }
        clients.clear();
        pump_until(**r, [&] {
            child.reap();
            return child.connections() == 0;
        });
        // As on SIGTERM: drain starts inside an iteration and the loop exits as soon as
        // nothing is left, without another trip into the kernel.
        child.begin_drain();
        child.reap();
        return child.finished() ? 0 : 5;
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
    ASSERT_TRUE(saturate(client.get()));
    server->begin_drain();
    advance(Millis{29'000});
    ulw::test::pump_for(*reactor, std::chrono::milliseconds(50));
    server->reap();
    EXPECT_EQ(server->connections(), 1U);
    advance(Millis{1'200});
    EXPECT_TRUE(settle([&] { return server->finished(); }));
}

// The idle timer (600 s) is clamped into the wheel's furthest slot, 51.1 s out. A drain begun
// 21.1 s in has its 30 s deadline land in that same slot, ahead of the idle timer, and closing
// the session from the deadline cancels the idle timer while the slot is being fired.
TEST_P(EchoServerTest, DrainDeadlineClosesASessionWhoseIdleTimerSharesItsSlot) {
    restart({.idle_timeout = Millis{600'000}});
    auto client = connect_loopback(port);
    ASSERT_TRUE(settle([&] { return server->connections() == 1; }));
    ASSERT_TRUE(saturate(client.get()));
    advance(Millis{21'100});
    server->begin_drain();
    advance(Millis{30'000});
    ASSERT_TRUE(settle([&] { return server->finished(); }));
    // The cancelled idle timer must be gone, not merely skipped for one pass.
    advance(Millis{600'000});
}

// io_uring tears a ring down asynchronously, and for a ring whose task has exited that can
// take a while; until then, requests still in flight keep their sockets, the listener
// included, alive. A restarted process must find its port free the moment the old one exits.
TEST_P(EchoServerTest, PortIsFreeTheMomentADrainedServerProcessExits) {
    server.reset();
    reactor.reset();
    for (int round = 0; round < 5; ++round) {
        const pid_t pid = ::fork();
        ASSERT_GE(pid, 0);
        if (pid == 0) {
            // Destructors must run, as they do when a real server returns from main.
            ::_exit(run_child_server());
        }
        int status = 0;
        ASSERT_EQ(::waitpid(pid, &status, 0), pid);
        ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << "child status " << status;
        auto again = net::listen_tcp({.port = port, .loopback_only = true});
        ASSERT_TRUE(again) << "round " << round << ": " << std::strerror(again.error());
    }
}

INSTANTIATE_TEST_SUITE_P(Reactors, EchoServerTest,
                         ::testing::Values(ReactorKind::IoUring, ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
