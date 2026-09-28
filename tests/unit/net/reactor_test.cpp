#include "net/reactor_factory.hpp"
#include "net/socket.hpp"
#include "os/system_clock.hpp"

#include "support/fake_clock.hpp"
#include "support/reactor_harness.hpp"

#include <sys/eventfd.h>
#include <sys/resource.h>

#include <cstring>
#include <functional>
#include <gtest/gtest.h>
#include <optional>
#include <unistd.h>
#include <vector>

namespace {

using core::Millis;
using net::ConnId;
using net::ReactorKind;
using ulw::test::connect_loopback;
using ulw::test::kKiB;
using ulw::test::kMiB;
using ulw::test::pattern;
using ulw::test::pump_for;
using ulw::test::pump_until;
using ulw::test::read_some;
using ulw::test::write_some;

constexpr std::size_t kSlots = 4096;

struct Acceptor final : net::IAcceptHandler {
    std::vector<os::UniqueFd> accepted;
    void on_accept(os::UniqueFd fd) noexcept override { accepted.push_back(std::move(fd)); }
};

struct Conn final : net::IStreamHandler {
    net::IReactor* reactor = nullptr;
    ConnId id;
    std::vector<std::byte> received;
    int data_calls = 0;
    int writable_calls = 0;
    bool eof = false;
    std::optional<int> error;
    std::function<void(Conn&, net::BorrowedBytes)> on_data_hook;

    void on_data(net::BorrowedBytes bytes) noexcept override {
        ++data_calls;
        received.insert(received.end(), bytes.begin(), bytes.end());
        if (on_data_hook) {
            on_data_hook(*this, bytes);
        }
    }
    void on_writable() noexcept override { ++writable_calls; }
    void on_peer_eof() noexcept override { eof = true; }
    void on_error(int err) noexcept override { error = err; }
};

class ReactorTest : public ::testing::TestWithParam<ReactorKind> {
protected:
    void SetUp() override {
        auto r = net::make_reactor(GetParam(), clock_, kSlots);
        ASSERT_TRUE(r) << "reactor setup failed: " << std::strerror(r.error());
        reactor = std::move(*r);
        auto listener = net::listen_tcp({.port = 0, .loopback_only = true});
        ASSERT_TRUE(listener);
        port = *net::local_port(listener->get());
        ASSERT_TRUE(reactor->listen(std::move(*listener), acceptor));
    }

    // Connects a client and attaches the accepted side to `server`.
    os::UniqueFd connect(Conn& server) {
        auto client = connect_loopback(port);
        EXPECT_TRUE(client);
        const std::size_t before = acceptor.accepted.size();
        EXPECT_TRUE(pump_until(*reactor, [&] { return acceptor.accepted.size() > before; }));
        server.reactor = reactor.get();
        auto id = reactor->attach(std::move(acceptor.accepted.back()), server);
        acceptor.accepted.pop_back();
        EXPECT_TRUE(id);
        server.id = *id;
        return client;
    }

    os::SystemClock clock_;
    std::unique_ptr<net::IReactor> reactor;
    Acceptor acceptor;
    std::uint16_t port = 0;
};

TEST_P(ReactorTest, EchoesBytesBack) {
    Conn server;
    server.on_data_hook = [](Conn& c, net::BorrowedBytes b) { c.reactor->send(c.id, b); };
    auto client = connect(server);
    reactor->start_receiving(server.id);

    const std::string_view msg = "hello, reactor";
    ASSERT_EQ(write_some(client.get(), std::as_bytes(std::span(msg))), msg.size());
    std::vector<std::byte> echoed;
    ASSERT_TRUE(pump_until(*reactor, [&] {
        read_some(client.get(), echoed);
        return echoed.size() >= msg.size();
    }));
    EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(echoed.data()), echoed.size()), msg);
}

TEST_P(ReactorTest, LargeTransferArrivesByteExactUnderBackpressure) {
    constexpr std::size_t kTotal = 8 * kMiB;
    constexpr std::size_t kHighWater = 256 * kKiB;
    struct Echo final : net::IStreamHandler {
        net::IReactor* reactor = nullptr;
        ConnId id;
        bool paused = false;
        std::size_t max_pending = 0;
        void on_data(net::BorrowedBytes b) noexcept override {
            reactor->send(id, b);
            const std::size_t pending = reactor->pending_send_bytes(id);
            max_pending = std::max(max_pending, pending);
            if (pending > kHighWater) {
                paused = true;
                reactor->stop_receiving(id);
            }
        }
        void on_writable() noexcept override {
            if (paused) {
                paused = false;
                reactor->start_receiving(id);
            }
        }
        void on_peer_eof() noexcept override {}
        void on_error(int /*err*/) noexcept override {}
    } echo;
    auto client = connect_loopback(port);
    ASSERT_TRUE(pump_until(*reactor, [&] { return !acceptor.accepted.empty(); }));
    echo.reactor = reactor.get();
    echo.id = *reactor->attach(std::move(acceptor.accepted.back()), echo);
    reactor->start_receiving(echo.id);

    const auto data = pattern(kTotal);
    std::size_t sent = 0;
    std::vector<std::byte> back;
    back.reserve(kTotal);
    ASSERT_TRUE(pump_until(
        *reactor,
        [&] {
            if (sent < kTotal) {
                sent += write_some(client.get(), std::span(data).subspan(sent));
            }
            read_some(client.get(), back);
            return back.size() >= kTotal;
        },
        std::chrono::seconds(60)));
    EXPECT_TRUE(back == data);
    // One on_data can add at most one receive buffer on top of the high-water mark.
    EXPECT_LE(echo.max_pending, kHighWater + (64 * kKiB));
}

TEST_P(ReactorTest, StopReceivingIsExactAndResumeDeliversTheRestInOrder) {
    Conn server;
    server.on_data_hook = [](Conn& c, net::BorrowedBytes) { c.reactor->stop_receiving(c.id); };
    auto client = connect(server);
    reactor->start_receiving(server.id);

    const auto data = pattern(kMiB);
    std::size_t sent = 0;
    ASSERT_TRUE(pump_until(*reactor, [&] {
        sent += write_some(client.get(), std::span(data).subspan(sent));
        return server.data_calls > 0;
    }));
    // Keep the kernel busy with more bytes than any receive buffer holds.
    pump_for(*reactor, std::chrono::milliseconds(200));
    sent += write_some(client.get(), std::span(data).subspan(sent));
    pump_for(*reactor, std::chrono::milliseconds(100));
    EXPECT_EQ(server.data_calls, 1);
    EXPECT_LE(server.received.size(), 64 * kKiB);

    server.on_data_hook = nullptr;
    reactor->start_receiving(server.id);
    ASSERT_TRUE(pump_until(*reactor, [&] {
        if (sent < data.size()) {
            sent += write_some(client.get(), std::span(data).subspan(sent));
        }
        return server.received.size() == data.size();
    }));
    EXPECT_TRUE(server.received == data);
}

TEST_P(ReactorTest, ResumingAfterPeerHalfCloseDeliversDataThenEof) {
    Conn server;
    server.on_data_hook = [](Conn& c, net::BorrowedBytes) { c.reactor->stop_receiving(c.id); };
    auto client = connect(server);
    reactor->start_receiving(server.id);

    const auto data = pattern(200 * kKiB);
    std::size_t sent = 0;
    ASSERT_TRUE(pump_until(*reactor, [&] {
        sent += write_some(client.get(), std::span(data).subspan(sent));
        return sent == data.size() && server.data_calls > 0;
    }));
    ::shutdown(client.get(), SHUT_WR);
    pump_for(*reactor, std::chrono::milliseconds(50));
    EXPECT_FALSE(server.eof);

    server.on_data_hook = nullptr;
    reactor->start_receiving(server.id);
    ASSERT_TRUE(pump_until(*reactor, [&] { return server.eof; }));
    EXPECT_TRUE(server.received == data);
}

TEST_P(ReactorTest, PeerResetIsReportedAsAnError) {
    Conn server;
    auto client = connect(server);
    reactor->start_receiving(server.id);
    linger lg{.l_onoff = 1, .l_linger = 0};
    ::setsockopt(client.get(), SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
    client.reset();
    ASSERT_TRUE(pump_until(*reactor, [&] { return server.error.has_value() || server.eof; }));
    ASSERT_TRUE(server.error.has_value());
    EXPECT_EQ(*server.error, ECONNRESET);
}

TEST_P(ReactorTest, StaleHandleCannotTouchTheConnectionThatReusedItsDescriptor) {
    Conn first;
    auto client1 = connect(first);
    const ConnId stale = first.id;
    // Queue the second connection in the backlog before the first closes, so the client
    // socket does not take the freed number and the accept does.
    auto client2 = connect_loopback(port);
    ASSERT_TRUE(client2);
    reactor->begin_close(stale);
    ASSERT_TRUE(pump_until(*reactor, [&] { return reactor->is_quiescent(stale); }));
    ASSERT_TRUE(pump_until(*reactor, [&] { return !acceptor.accepted.empty(); }));

    Conn second;
    second.reactor = reactor.get();
    second.id = *reactor->attach(std::move(acceptor.accepted.back()), second);
    ASSERT_EQ(second.id.fd, stale.fd) << "kernel did not reuse the descriptor number";
    ASSERT_NE(second.id.gen, stale.gen);

    const std::string_view msg = "not for you";
    reactor->send(stale, std::as_bytes(std::span(msg)));
    reactor->stop_receiving(stale);
    reactor->begin_close(stale);
    reactor->start_receiving(second.id);
    pump_for(*reactor, std::chrono::milliseconds(50));

    std::vector<std::byte> got;
    read_some(client2.get(), got);
    EXPECT_TRUE(got.empty());
    EXPECT_FALSE(reactor->is_quiescent(second.id));
    ASSERT_EQ(write_some(client2.get(), std::as_bytes(std::span(msg))), msg.size());
    ASSERT_TRUE(pump_until(*reactor, [&] { return second.received.size() == msg.size(); }));
}

TEST_P(ReactorTest, NoCallbackFollowsCloseFromInsideACallback) {
    Conn server;
    server.on_data_hook = [](Conn& c, net::BorrowedBytes) { c.reactor->begin_close(c.id); };
    auto client = connect(server);
    reactor->start_receiving(server.id);
    const auto data = pattern(512 * kKiB);
    std::size_t sent = 0;
    ASSERT_TRUE(pump_until(*reactor, [&] {
        sent += write_some(client.get(), std::span(data).subspan(sent));
        return server.data_calls > 0;
    }));
    client.reset();
    ASSERT_TRUE(pump_until(*reactor, [&] { return reactor->is_quiescent(server.id); }));
    pump_for(*reactor, std::chrono::milliseconds(50));
    EXPECT_EQ(server.data_calls, 1);
    EXPECT_FALSE(server.eof);
    EXPECT_FALSE(server.error.has_value());
}

TEST_P(ReactorTest, SendQueueToASlowReaderDrainsAndReportsWritable) {
    Conn server;
    auto client = connect(server);
    const auto data = pattern(4 * kMiB);
    reactor->send(server.id, data);
    pump_for(*reactor, std::chrono::milliseconds(20));
    ASSERT_GT(reactor->pending_send_bytes(server.id), 0U);
    EXPECT_EQ(server.writable_calls, 0);

    std::vector<std::byte> got;
    ASSERT_TRUE(pump_until(*reactor, [&] {
        read_some(client.get(), got);
        return got.size() == data.size() && server.writable_calls > 0;
    }));
    EXPECT_EQ(reactor->pending_send_bytes(server.id), 0U);
    EXPECT_TRUE(got == data);
}

TEST_P(ReactorTest, SendQueueBeyondItsCapFailsTheConnection) {
    Conn server;
    auto client = connect(server);
    const auto chunk = pattern(kMiB);
    for (int i = 0; i < 8 && !server.error; ++i) {
        reactor->send(server.id, chunk);
        reactor->run_once(Millis{0});
    }
    ASSERT_TRUE(pump_until(*reactor, [&] { return server.error.has_value(); }));
    EXPECT_EQ(*server.error, ENOBUFS);
}

TEST_P(ReactorTest, AcceptsManyConnections) {
    constexpr std::size_t kClients = 200;
    std::vector<os::UniqueFd> clients;
    for (std::size_t i = 0; i < kClients; ++i) {
        clients.push_back(connect_loopback(port));
        ASSERT_TRUE(clients.back());
    }
    ASSERT_TRUE(pump_until(*reactor, [&] { return acceptor.accepted.size() == kClients; }));
}

TEST_P(ReactorTest, StopListeningRefusesNewConnections) {
    reactor->stop_listening();
    pump_for(*reactor, std::chrono::milliseconds(20));
    EXPECT_FALSE(connect_loopback(port));
}

TEST_P(ReactorTest, WatchedDescriptorReportsReadinessUntilUnwatched) {
    struct Ready final : net::IReadyHandler {
        int fd = -1;
        int calls = 0;
        net::Interest last = net::Interest::None;
        void on_ready(net::Interest i) noexcept override {
            ++calls;
            last = i;
            std::uint64_t v = 0;
            if (::read(fd, &v, sizeof v) < 0) {
                return;
            }
        }
    } ready;
    const os::UniqueFd efd{::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)};
    ready.fd = efd.get();
    ASSERT_TRUE(reactor->watch(efd.get(), net::Interest::Read, ready));
    pump_for(*reactor, std::chrono::milliseconds(20));
    EXPECT_EQ(ready.calls, 0);

    const std::uint64_t one = 1;
    ASSERT_EQ(::write(efd.get(), &one, sizeof one), 8);
    ASSERT_TRUE(pump_until(*reactor, [&] { return ready.calls == 1; }));
    EXPECT_TRUE(net::has(ready.last, net::Interest::Read));
    ASSERT_EQ(::write(efd.get(), &one, sizeof one), 8);
    ASSERT_TRUE(pump_until(*reactor, [&] { return ready.calls == 2; }));

    reactor->unwatch(efd.get());
    ASSERT_EQ(::write(efd.get(), &one, sizeof one), 8);
    pump_for(*reactor, std::chrono::milliseconds(50));
    EXPECT_EQ(ready.calls, 2);
}

TEST_P(ReactorTest, ChangingWatchInterestTakesEffect) {
    struct Ready final : net::IReadyHandler {
        int calls = 0;
        net::Interest last = net::Interest::None;
        void on_ready(net::Interest i) noexcept override {
            ++calls;
            last = i;
        }
    } ready;
    const os::UniqueFd efd{::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)};
    ASSERT_TRUE(reactor->watch(efd.get(), net::Interest::Read, ready));
    pump_for(*reactor, std::chrono::milliseconds(20));
    ASSERT_EQ(ready.calls, 0);
    // An eventfd with a zero counter is always writable.
    ASSERT_TRUE(reactor->watch(efd.get(), net::Interest::Write, ready));
    ASSERT_TRUE(pump_until(*reactor, [&] { return ready.calls > 0; }));
    EXPECT_TRUE(net::has(ready.last, net::Interest::Write));
    reactor->unwatch(efd.get());
}

class ReactorTimerTest : public ::testing::TestWithParam<ReactorKind> {
protected:
    void SetUp() override {
        auto r = net::make_reactor(GetParam(), clock, kSlots);
        ASSERT_TRUE(r);
        reactor = std::move(*r);
    }
    void advance(Millis d) {
        clock.advance(d);
        reactor->run_once(Millis{0});
    }
    ulw::test::FakeClock clock;
    std::unique_ptr<net::IReactor> reactor;
};

struct Counter final : net::ITimerHandler {
    int fired = 0;
    void on_timeout() noexcept override { ++fired; }
};

TEST_P(ReactorTimerTest, TimerFiresFromTheLoopOnceItsDeadlinePasses) {
    Counter c;
    reactor->arm_timer(Millis{10'000}, c);
    advance(Millis{9'900});
    EXPECT_EQ(c.fired, 0);
    advance(Millis{200});
    EXPECT_EQ(c.fired, 1);
    advance(Millis{20'000});
    EXPECT_EQ(c.fired, 1);
}

TEST_P(ReactorTimerTest, CancelledTimerDoesNotFire) {
    Counter c;
    const auto id = reactor->arm_timer(Millis{100}, c);
    reactor->cancel_timer(id);
    advance(Millis{1'000});
    EXPECT_EQ(c.fired, 0);
}

TEST_P(ReactorTimerTest, NowAdvancesOncePerIteration) {
    const auto before = reactor->now();
    clock.advance(Millis{1'234});
    EXPECT_EQ(reactor->now(), before);
    reactor->run_once(Millis{0});
    EXPECT_EQ(reactor->now(), clock.now());
}

// Descriptor exhaustion must not turn a readable listener into a busy loop.
TEST_P(ReactorTimerTest, ListenerPausesOnDescriptorExhaustionAndResumes) {
    Acceptor acceptor;
    auto listener = net::listen_tcp({.port = 0, .loopback_only = true});
    ASSERT_TRUE(listener);
    const std::uint16_t port = *net::local_port(listener->get());
    ASSERT_TRUE(reactor->listen(std::move(*listener), acceptor));

    std::vector<os::UniqueFd> clients;
    for (int i = 0; i < 4; ++i) {
        clients.push_back(connect_loopback(port));
        ASSERT_TRUE(clients.back());
    }
    rlimit saved{};
    ::getrlimit(RLIMIT_NOFILE, &saved);
    os::UniqueFd probe{::eventfd(0, EFD_CLOEXEC)};
    rlimit tight = saved;
    tight.rlim_cur = static_cast<rlim_t>(probe.get());
    probe.reset();
    ::setrlimit(RLIMIT_NOFILE, &tight);

    int events = 0;
    for (int i = 0; i < 50; ++i) {
        events += reactor->run_once(Millis{2});
    }
    ::setrlimit(RLIMIT_NOFILE, &saved);
    EXPECT_TRUE(acceptor.accepted.empty());
    // A spinning listener would report an event on every iteration.
    EXPECT_LT(events, 10);

    advance(Millis{200});
    ASSERT_TRUE(pump_until(*reactor, [&] { return acceptor.accepted.size() == clients.size(); }));
}

const auto kKinds = ::testing::Values(ReactorKind::IoUring, ReactorKind::Epoll);
const auto kName = [](const ::testing::TestParamInfo<ReactorKind>& info) {
    return std::string(info.param == ReactorKind::IoUring ? "IoUring" : "Epoll");
};

INSTANTIATE_TEST_SUITE_P(Reactors, ReactorTest, kKinds, kName);
INSTANTIATE_TEST_SUITE_P(Reactors, ReactorTimerTest, kKinds, kName);

} // namespace
