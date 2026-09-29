#include "net/reactor_factory.hpp"
#include "net/socket.hpp"
#include "os/system_clock.hpp"

#include "sockaddr.hpp"
#include "support/fake_random.hpp"
#include "support/reactor_harness.hpp"

#include <sys/socket.h>

#include <array>
#include <cstring>
#include <functional>
#include <gtest/gtest.h>
#include <map>
#include <optional>
#include <vector>

namespace {

using net::AddrFamily;
using net::DatagramId;
using net::ReactorKind;
using net::SocketAddr;
using ulw::test::pump_pending;
using ulw::test::pump_until;

constexpr std::size_t kSlots = 4096;

struct Received {
    SocketAddr from;
    std::vector<std::byte> payload;
};

struct Sink final : net::IDatagramHandler {
    net::IReactor* reactor = nullptr;
    DatagramId id;
    std::vector<Received> got;
    std::vector<std::pair<SocketAddr, int>> send_errors;
    std::optional<int> error;
    std::function<void(Sink&)> after_datagram;

    void on_datagram(SocketAddr from, net::BorrowedBytes payload) noexcept override {
        got.push_back({.from = from, .payload = {payload.begin(), payload.end()}});
        if (after_datagram) {
            after_datagram(*this);
        }
    }
    void on_send_error(SocketAddr to, int err) noexcept override {
        send_errors.emplace_back(to, err);
    }
    void on_error(int err) noexcept override { error = err; }
};

// A peer outside the reactor, driven with plain syscalls.
struct Peer {
    os::UniqueFd fd;
    SocketAddr addr;

    static Peer bind(AddrFamily family) {
        Peer p;
        auto fd = net::bind_udp(SocketAddr::loopback(family, 0));
        if (!fd) {
            ADD_FAILURE() << "bind_udp: " << std::strerror(fd.error());
            return p;
        }
        p.fd = std::move(*fd);
        p.addr = *net::local_addr(p.fd.get());
        return p;
    }

    [[nodiscard]] bool send(const SocketAddr& to, std::span<const std::byte> payload) const {
        sockaddr_storage raw{};
        const auto len = net::detail::to_sockaddr(to, to.family == AddrFamily::V6, raw);
        // sendto takes every address family through the generic sockaddr header.
        const auto* dst = reinterpret_cast<const sockaddr*>(&raw); // NOLINT(*-reinterpret-cast)
        return len && ::sendto(fd.get(), payload.data(), payload.size(), 0, dst, *len) ==
                          static_cast<ssize_t>(payload.size());
    }

    [[nodiscard]] std::vector<std::vector<std::byte>> drain() const {
        std::vector<std::vector<std::byte>> out;
        std::array<std::byte, 4096> buf{};
        for (;;) {
            const ssize_t n = ::recv(fd.get(), buf.data(), buf.size(), MSG_DONTWAIT);
            if (n < 0) {
                return out;
            }
            out.emplace_back(buf.begin(), buf.begin() + n);
        }
    }
};

// Payload of the attribution tests: which source sent it and in what order.
struct Tag {
    std::uint64_t nonce = 0;
    std::uint64_t seq = 0;
};

std::array<std::byte, sizeof(Tag)> encode(Tag t) {
    std::array<std::byte, sizeof(Tag)> out{};
    std::memcpy(out.data(), &t, sizeof t);
    return out;
}

Tag decode(std::span<const std::byte> bytes) {
    Tag t;
    if (bytes.size() == sizeof t) {
        std::memcpy(&t, bytes.data(), sizeof t);
    }
    return t;
}

bool ipv6_available() {
    return static_cast<bool>(net::bind_udp(SocketAddr::loopback(AddrFamily::V6, 0)));
}

class DatagramTest : public ::testing::TestWithParam<ReactorKind> {
protected:
    void SetUp() override {
        auto r = net::make_reactor(GetParam(), clock_, kSlots);
        ASSERT_TRUE(r) << "reactor setup failed: " << std::strerror(r.error());
        reactor = std::move(*r);
    }

    // Returns the socket's address.
    SocketAddr attach(Sink& sink, const SocketAddr& local) {
        auto fd = net::bind_udp(local);
        if (!fd) {
            ADD_FAILURE() << "bind_udp: " << std::strerror(fd.error());
            return {};
        }
        auto addr = *net::local_addr(fd->get());
        auto id = reactor->attach_datagram(std::move(*fd), sink);
        EXPECT_TRUE(id);
        sink.reactor = reactor.get();
        sink.id = id.value_or(DatagramId{});
        return addr;
    }

    SocketAddr attach(Sink& sink) { return attach(sink, SocketAddr::loopback(AddrFamily::V4, 0)); }

    os::SystemClock clock_;
    std::unique_ptr<net::IReactor> reactor;
};

TEST_P(DatagramTest, DeliversDatagramWithItsSourceAndRepliesReachThatSource) {
    Sink sink;
    sink.after_datagram = [](Sink& s) {
        const Received& r = s.got.back();
        EXPECT_TRUE(s.reactor->send_to(s.id, r.from, r.payload));
    };
    const SocketAddr server = attach(sink);
    reactor->start_receiving_datagrams(sink.id);
    const Peer peer = Peer::bind(AddrFamily::V4);

    const auto msg = ulw::test::pattern(1200);
    ASSERT_TRUE(peer.send(server, msg));
    ASSERT_TRUE(pump_until(*reactor, [&] { return !sink.got.empty(); }));
    EXPECT_EQ(sink.got[0].from, peer.addr);
    EXPECT_EQ(sink.got[0].payload, msg);

    std::vector<std::vector<std::byte>> echoed;
    ASSERT_TRUE(pump_until(*reactor, [&] {
        for (auto& d : peer.drain()) {
            echoed.push_back(std::move(d));
        }
        return !echoed.empty();
    }));
    EXPECT_EQ(echoed[0], msg);
    const auto stats = reactor->datagram_stats(sink.id);
    EXPECT_EQ(stats.received, 1U);
    EXPECT_EQ(stats.sent, 1U);
}

TEST_P(DatagramTest, AttributesEveryDatagramOf500SourcesToItsSender) {
    constexpr std::size_t kSources = 500;
    constexpr std::uint64_t kPerSource = 4;
    // Sent in rounds of 100: well inside the 256 small datagrams a default receive buffer
    // holds, so the kernel drops nothing and every datagram can be accounted for.
    constexpr std::size_t kRound = 100;
    Sink sink;
    const SocketAddr server = attach(sink);
    reactor->start_receiving_datagrams(sink.id);

    ulw::test::FakeRandom random;
    std::vector<Peer> peers;
    peers.reserve(kSources);
    std::map<std::uint16_t, std::uint64_t> nonce_of_port;
    for (std::size_t i = 0; i < kSources; ++i) {
        peers.push_back(Peer::bind(AddrFamily::V4));
        std::uint64_t nonce = 0;
        random.fill(std::as_writable_bytes(std::span(&nonce, 1)));
        nonce_of_port[peers.back().addr.port] = nonce;
    }
    ASSERT_EQ(nonce_of_port.size(), kSources);

    std::size_t sent = 0;
    for (std::uint64_t seq = 0; seq < kPerSource; ++seq) {
        for (std::size_t first = 0; first < kSources; first += kRound) {
            for (std::size_t i = first; i < first + kRound; ++i) {
                const Tag tag{.nonce = nonce_of_port[peers[i].addr.port], .seq = seq};
                ASSERT_TRUE(peers[i].send(server, encode(tag)));
            }
            sent += kRound;
            ASSERT_TRUE(pump_until(*reactor, [&] { return sink.got.size() == sent; }));
        }
    }

    std::map<std::uint16_t, std::uint64_t> next_seq;
    for (const Received& r : sink.got) {
        ASSERT_EQ(r.from.family, AddrFamily::V4);
        ASSERT_EQ(r.from, SocketAddr::loopback(AddrFamily::V4, r.from.port));
        const Tag tag = decode(r.payload);
        ASSERT_EQ(tag.nonce, nonce_of_port.at(r.from.port)) << "misattributed datagram";
        EXPECT_EQ(tag.seq, next_seq[r.from.port]++);
    }
    EXPECT_EQ(next_seq.size(), kSources);
    EXPECT_EQ(reactor->datagram_stats(sink.id).received, kSources * kPerSource);
}

// Loopback preserves the order of one sender's datagrams, so any reordering is the reactor's.
TEST_P(DatagramTest, KeepsTheOrderOfOneSourceAcrossBatches) {
    constexpr std::uint64_t kTotal = 1000;
    constexpr std::uint64_t kBurst = 200;
    Sink sink;
    const SocketAddr server = attach(sink);
    reactor->start_receiving_datagrams(sink.id);
    const Peer peer = Peer::bind(AddrFamily::V4);

    for (std::uint64_t seq = 0; seq < kTotal; ++seq) {
        ASSERT_TRUE(peer.send(server, encode({.nonce = 7, .seq = seq})));
        if ((seq + 1) % kBurst == 0) {
            ASSERT_TRUE(pump_until(*reactor, [&] { return sink.got.size() == seq + 1; }));
        }
    }
    for (std::uint64_t i = 0; i < kTotal; ++i) {
        ASSERT_EQ(decode(sink.got[i].payload).seq, i);
    }
}

TEST_P(DatagramTest, DropsAndCountsDatagramsLongerThanTheLimit) {
    Sink sink;
    const SocketAddr server = attach(sink);
    reactor->start_receiving_datagrams(sink.id);
    const Peer peer = Peer::bind(AddrFamily::V4);

    const auto longest = ulw::test::pattern(net::kMaxDatagramSize, 1);
    const auto too_long = ulw::test::pattern(net::kMaxDatagramSize + 1, 2);
    const auto far_too_long = ulw::test::pattern(60'000, 3);
    ASSERT_TRUE(peer.send(server, too_long));
    ASSERT_TRUE(peer.send(server, longest));
    ASSERT_TRUE(peer.send(server, far_too_long));
    ASSERT_TRUE(peer.send(server, std::span<const std::byte>{}));
    ASSERT_TRUE(pump_until(*reactor, [&] { return sink.got.size() == 2; }));
    pump_pending(*reactor);

    ASSERT_EQ(sink.got.size(), 2U);
    EXPECT_EQ(sink.got[0].payload, longest);
    EXPECT_TRUE(sink.got[1].payload.empty());
    const auto stats = reactor->datagram_stats(sink.id);
    EXPECT_EQ(stats.truncated, 2U);
    EXPECT_EQ(stats.received, 2U);
}

TEST_P(DatagramTest, StopIsExactAndResumeDeliversWhatWaitedInOrder) {
    constexpr std::uint64_t kBurst = 200;
    constexpr std::uint64_t kStopAfter = 3;
    Sink sink;
    sink.after_datagram = [&](Sink& s) {
        if (s.got.size() == kStopAfter) {
            s.reactor->stop_receiving_datagrams(s.id);
        }
    };
    const SocketAddr server = attach(sink);
    const Peer peer = Peer::bind(AddrFamily::V4);
    // Queued before the first receive is armed, so the kernel has a batch ready at once.
    for (std::uint64_t seq = 0; seq < kBurst; ++seq) {
        ASSERT_TRUE(peer.send(server, encode({.nonce = 1, .seq = seq})));
    }
    reactor->start_receiving_datagrams(sink.id);
    ASSERT_TRUE(pump_until(*reactor, [&] { return sink.got.size() >= kStopAfter; }));
    pump_pending(*reactor);
    EXPECT_EQ(sink.got.size(), kStopAfter);

    sink.after_datagram = nullptr;
    reactor->start_receiving_datagrams(sink.id);
    ASSERT_TRUE(pump_until(*reactor, [&] { return sink.got.size() == kBurst; }));
    for (std::uint64_t i = 0; i < kBurst; ++i) {
        ASSERT_EQ(decode(sink.got[i].payload).seq, i);
    }
}

TEST_P(DatagramTest, StopAndStartWithinOneCallbackLosesNothing) {
    constexpr std::uint64_t kBurst = 100;
    Sink sink;
    sink.after_datagram = [](Sink& s) {
        s.reactor->stop_receiving_datagrams(s.id);
        s.reactor->start_receiving_datagrams(s.id);
    };
    const SocketAddr server = attach(sink);
    const Peer peer = Peer::bind(AddrFamily::V4);
    for (std::uint64_t seq = 0; seq < kBurst; ++seq) {
        ASSERT_TRUE(peer.send(server, encode({.nonce = 1, .seq = seq})));
    }
    reactor->start_receiving_datagrams(sink.id);
    ASSERT_TRUE(pump_until(*reactor, [&] { return sink.got.size() == kBurst; }));
    for (std::uint64_t i = 0; i < kBurst; ++i) {
        ASSERT_EQ(decode(sink.got[i].payload).seq, i);
    }
}

TEST_P(DatagramTest, NoCallbackFollowsCloseFromInsideOnDatagram) {
    constexpr std::uint64_t kBurst = 100;
    Sink sink;
    sink.after_datagram = [](Sink& s) {
        if (s.got.size() == 5) {
            s.reactor->begin_close(s.id);
        }
    };
    const SocketAddr server = attach(sink);
    const Peer peer = Peer::bind(AddrFamily::V4);
    for (std::uint64_t seq = 0; seq < kBurst; ++seq) {
        ASSERT_TRUE(peer.send(server, encode({.nonce = 1, .seq = seq})));
    }
    reactor->start_receiving_datagrams(sink.id);
    ASSERT_TRUE(pump_until(*reactor, [&] { return reactor->is_quiescent(sink.id); }));
    pump_pending(*reactor);
    EXPECT_EQ(sink.got.size(), 5U);
    EXPECT_EQ(reactor->send_to(sink.id, peer.addr, encode({})).error_or(0), EBADF);
}

// Datagrams that arrive after a stop wait in receive buffers. Closing the socket must give
// every one back, or a few rounds of this starve every other socket on the reactor.
TEST_P(DatagramTest, ClosingAStoppedSocketGivesBackWhatWaited) {
    constexpr std::uint64_t kBurst = 250;
    const Peer peer = Peer::bind(AddrFamily::V4);
    for (int round = 0; round < 8; ++round) {
        Sink stopped;
        stopped.after_datagram = [](Sink& s) { s.reactor->stop_receiving_datagrams(s.id); };
        const SocketAddr server = attach(stopped);
        for (std::uint64_t seq = 0; seq < kBurst; ++seq) {
            ASSERT_TRUE(peer.send(server, encode({.nonce = 1, .seq = seq})));
        }
        reactor->start_receiving_datagrams(stopped.id);
        ASSERT_TRUE(pump_until(*reactor, [&] { return !stopped.got.empty(); }));
        pump_pending(*reactor);
        reactor->begin_close(stopped.id);
        ASSERT_TRUE(pump_until(*reactor, [&] { return reactor->is_quiescent(stopped.id); }));
    }

    Sink live;
    const SocketAddr server = attach(live);
    reactor->start_receiving_datagrams(live.id);
    std::uint64_t sent = 0;
    for (int round = 0; round < 8; ++round) {
        for (std::uint64_t i = 0; i < kBurst; ++i, ++sent) {
            ASSERT_TRUE(peer.send(server, encode({.nonce = 2, .seq = sent})));
        }
        ASSERT_TRUE(pump_until(*reactor, [&] { return live.got.size() == sent; }));
    }
    EXPECT_EQ(reactor->datagram_stats(live.id).received, sent);
}

// Many sockets with full receive buffers armed at once can ask for more buffers than the ring
// holds. What does not fit waits in the kernel and still arrives, in order.
TEST_P(DatagramTest, ManySocketsDrainingAtOnceLoseNothing) {
    constexpr std::size_t kSockets = 24;
    constexpr std::uint64_t kEach = 200;
    std::vector<Sink> sinks(kSockets);
    std::vector<SocketAddr> addrs;
    addrs.reserve(kSockets);
    for (Sink& s : sinks) {
        addrs.push_back(attach(s));
    }
    const Peer peer = Peer::bind(AddrFamily::V4);
    for (std::size_t i = 0; i < kSockets; ++i) {
        for (std::uint64_t seq = 0; seq < kEach; ++seq) {
            ASSERT_TRUE(peer.send(addrs[i], encode({.nonce = i, .seq = seq})));
        }
    }
    for (const Sink& s : sinks) {
        reactor->start_receiving_datagrams(s.id);
    }
    ASSERT_TRUE(pump_until(*reactor, [&] {
        return std::ranges::all_of(sinks, [&](const Sink& s) { return s.got.size() == kEach; });
    }));
    std::uint64_t exhausted = 0;
    for (std::size_t i = 0; i < kSockets; ++i) {
        for (std::uint64_t seq = 0; seq < kEach; ++seq) {
            const Tag tag = decode(sinks[i].got[seq].payload);
            ASSERT_EQ(tag.nonce, i);
            ASSERT_EQ(tag.seq, seq);
        }
        exhausted += reactor->datagram_stats(sinks[i].id).ring_exhausted;
    }
    // Otherwise the test did not exercise what it is named for.
    if (GetParam() == ReactorKind::IoUring) {
        EXPECT_GT(exhausted, 0U);
    }
}

// Datagrams that arrive for a stopped socket wait in receive buffers, and enough of them can
// hold every buffer the ring has. A socket still receiving must then wait for one to come back,
// not retry a receive that fails at once on every iteration.
TEST_P(DatagramTest, HeldDatagramsNeverMakeTheLoopSpin) {
    constexpr std::size_t kStopped = 8;
    constexpr std::uint64_t kEach = 250;
    const Peer peer = Peer::bind(AddrFamily::V4);
    std::vector<Sink> stopped(kStopped);
    std::vector<SocketAddr> addrs;
    addrs.reserve(kStopped);
    for (std::size_t i = 0; i < kStopped; ++i) {
        addrs.push_back(attach(stopped[i]));
        for (std::uint64_t seq = 0; seq < kEach; ++seq) {
            ASSERT_TRUE(peer.send(addrs[i], encode({.nonce = i, .seq = seq})));
        }
    }
    // The stop comes from a stream's callback, whose buffer belongs to another pool, so that
    // every datagram buffer the kernel fills in that batch stays held.
    struct Stopper final : net::IStreamHandler {
        net::IReactor* reactor = nullptr;
        std::vector<Sink>* sinks = nullptr;
        bool fired = false;
        void on_data(net::BorrowedBytes /*bytes*/) noexcept override {
            if (!fired) {
                fired = true;
                for (const Sink& s : *sinks) {
                    reactor->stop_receiving_datagrams(s.id);
                }
            }
        }
        void on_writable() noexcept override {}
        void on_peer_eof() noexcept override {}
        void on_error(int /*err*/) noexcept override {}
    } stopper;
    stopper.reactor = reactor.get();
    stopper.sinks = &stopped;
    auto [ours, theirs] = ulw::test::unix_pair();
    auto conn = reactor->attach(std::move(ours), stopper);
    ASSERT_TRUE(conn);
    ASSERT_EQ(ulw::test::write_some(theirs.get(), encode({})), sizeof(Tag));
    reactor->start_receiving(*conn);
    for (const Sink& s : stopped) {
        reactor->start_receiving_datagrams(s.id);
    }
    ASSERT_TRUE(pump_until(*reactor, [&] { return stopper.fired; }));
    pump_pending(*reactor);

    Sink live;
    const SocketAddr addr = attach(live);
    for (std::uint64_t seq = 0; seq < kEach; ++seq) {
        ASSERT_TRUE(peer.send(addr, encode({.nonce = kStopped, .seq = seq})));
    }
    reactor->start_receiving_datagrams(live.id);
    int events = 0;
    for (int i = 0; i < 50; ++i) {
        events += reactor->run_once(core::Millis{2});
    }
    // A spinning receive reports a completion on every iteration.
    EXPECT_LT(events, 20);
    if (GetParam() == ReactorKind::IoUring) {
        // Otherwise the test did not reach the state it is named for.
        EXPECT_TRUE(live.got.empty());
        EXPECT_GT(reactor->datagram_stats(live.id).ring_exhausted, 0U);
    }

    for (const Sink& s : stopped) {
        reactor->start_receiving_datagrams(s.id);
    }
    ASSERT_TRUE(pump_until(*reactor, [&] {
        return live.got.size() == kEach &&
               std::ranges::all_of(stopped, [&](const Sink& s) { return s.got.size() == kEach; });
    }));
    for (std::size_t i = 0; i <= kStopped; ++i) {
        const Sink& s = i < kStopped ? stopped[i] : live;
        for (std::uint64_t seq = 0; seq < kEach; ++seq) {
            const Tag tag = decode(s.got[seq].payload);
            ASSERT_EQ(tag.nonce, i);
            ASSERT_EQ(tag.seq, seq);
        }
    }
    reactor->begin_close(*conn);
}

TEST_P(DatagramTest, KernelRefusalArrivesThroughOnSendErrorFromTheLoop) {
    Sink sink;
    bool inside_send_to = false;
    std::optional<bool> reported_inside;
    struct Probe final : net::IDatagramHandler {
        Sink* sink = nullptr;
        bool* inside = nullptr;
        std::optional<bool>* reported_inside = nullptr;
        void on_datagram(SocketAddr from, net::BorrowedBytes b) noexcept override {
            sink->on_datagram(from, b);
        }
        void on_send_error(SocketAddr to, int err) noexcept override {
            *reported_inside = *inside;
            sink->on_send_error(to, err);
        }
        void on_error(int err) noexcept override { sink->on_error(err); }
    } probe;
    probe.sink = &sink;
    probe.inside = &inside_send_to;
    probe.reported_inside = &reported_inside;

    auto fd = net::bind_udp(SocketAddr::loopback(AddrFamily::V4, 0));
    ASSERT_TRUE(fd);
    auto id = reactor->attach_datagram(std::move(*fd), probe);
    ASSERT_TRUE(id);
    // Broadcast without SO_BROADCAST: the kernel refuses with EACCES.
    const SocketAddr broadcast = SocketAddr::v4({255, 255, 255, 255}, 9);
    inside_send_to = true;
    const auto accepted = reactor->send_to(*id, broadcast, encode({}));
    inside_send_to = false;
    ASSERT_TRUE(accepted);
    ASSERT_TRUE(pump_until(*reactor, [&] { return !sink.send_errors.empty(); }));
    EXPECT_EQ(reported_inside, false);
    EXPECT_EQ(sink.send_errors[0].first, broadcast);
    EXPECT_EQ(sink.send_errors[0].second, EACCES);
    EXPECT_EQ(reactor->datagram_stats(*id).send_errors, 1U);
    EXPECT_EQ(reactor->datagram_stats(*id).sent, 0U);
}

TEST_P(DatagramTest, SendToRejectsWhatItCanJudgeOnTheSpot) {
    Sink sink;
    attach(sink);
    const Peer peer = Peer::bind(AddrFamily::V4);
    const auto too_long = ulw::test::pattern(net::kMaxDatagramSize + 1);
    EXPECT_EQ(reactor->send_to(sink.id, peer.addr, too_long).error_or(0), EMSGSIZE);
    EXPECT_EQ(
        reactor->send_to(sink.id, SocketAddr::loopback(AddrFamily::V6, 9), encode({})).error_or(0),
        EAFNOSUPPORT);
    pump_pending(*reactor);
    EXPECT_TRUE(sink.send_errors.empty());
    EXPECT_TRUE(peer.drain().empty());
}

// send_to never queues without bound: a datagram is either taken, in which case it is sent,
// or refused with EAGAIN. io_uring refuses past its in-flight limit when the loop does not run;
// epoll hands each datagram to the kernel at once, so its refusals come only from a full kernel
// send buffer, which loopback never has.
TEST_P(DatagramTest, SendToTakesOrRefusesWithEagainAndSendsEverythingItTook) {
    constexpr std::uint64_t kAttempts = 2000;
    Sink sink;
    attach(sink);
    const Peer peer = Peer::bind(AddrFamily::V4);
    std::uint64_t taken = 0;
    std::uint64_t refused = 0;
    for (std::uint64_t i = 0; i < kAttempts; ++i) {
        const auto r = reactor->send_to(sink.id, peer.addr, encode({.nonce = 3, .seq = i}));
        if (r) {
            ++taken;
        } else {
            ASSERT_EQ(r.error(), EAGAIN);
            ++refused;
        }
    }
    ASSERT_TRUE(
        pump_until(*reactor, [&] { return reactor->datagram_stats(sink.id).sent == taken; }));
    const auto stats = reactor->datagram_stats(sink.id);
    EXPECT_EQ(stats.send_refused, refused);
    EXPECT_EQ(stats.send_errors, 0U);
    if (GetParam() == ReactorKind::IoUring) {
        EXPECT_GT(refused, 0U);
        EXPECT_LT(taken, kAttempts);
    }
    // Once the loop has run, the socket takes datagrams again.
    EXPECT_TRUE(reactor->send_to(sink.id, peer.addr, encode({})));
}

TEST_P(DatagramTest, QueuedSocketErrorStopsReceivingUntilRestarted) {
    Sink sink;
    auto fd = net::bind_udp(SocketAddr::loopback(AddrFamily::V4, 0));
    ASSERT_TRUE(fd);
    // A port nobody listens on, found by binding it and letting go.
    std::uint16_t closed_port = 0;
    {
        const Peer gone = Peer::bind(AddrFamily::V4);
        closed_port = gone.addr.port;
    }
    // Connected, so the kernel queues the ICMP port-unreachable as a socket error.
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(closed_port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): see Peer::send.
    ASSERT_EQ(::connect(fd->get(), reinterpret_cast<const sockaddr*>(&to), sizeof to), 0);
    const SocketAddr server = *net::local_addr(fd->get());
    auto id = reactor->attach_datagram(std::move(*fd), sink);
    ASSERT_TRUE(id);
    sink.reactor = reactor.get();
    sink.id = *id;
    reactor->start_receiving_datagrams(*id);

    const SocketAddr closed = SocketAddr::loopback(AddrFamily::V4, closed_port);
    ASSERT_TRUE(reactor->send_to(*id, closed, encode({})));
    ASSERT_TRUE(pump_until(*reactor, [&] { return sink.error.has_value(); }));
    EXPECT_EQ(sink.error, ECONNREFUSED);

    // The port comes back to life; its datagram waits until receiving is restarted.
    auto revived = net::bind_udp(closed);
    ASSERT_TRUE(revived);
    const Peer peer{.fd = std::move(*revived), .addr = closed};
    ASSERT_TRUE(peer.send(server, encode({.nonce = 9, .seq = 0})));
    pump_pending(*reactor);
    EXPECT_TRUE(sink.got.empty());

    reactor->start_receiving_datagrams(*id);
    ASSERT_TRUE(pump_until(*reactor, [&] { return !sink.got.empty(); }));
    EXPECT_EQ(decode(sink.got[0].payload).nonce, 9U);
    EXPECT_EQ(sink.got[0].from, closed);
}

TEST_P(DatagramTest, StaleIdCannotTouchTheSocketThatReusedItsDescriptor) {
    Sink first;
    attach(first);
    const DatagramId stale = first.id;
    reactor->begin_close(stale);
    ASSERT_TRUE(pump_until(*reactor, [&] { return reactor->is_quiescent(stale); }));

    Sink second;
    const SocketAddr server = attach(second);
    ASSERT_EQ(second.id.fd, stale.fd);
    reactor->start_receiving_datagrams(second.id);
    reactor->stop_receiving_datagrams(stale);
    reactor->begin_close(stale);
    const Peer peer = Peer::bind(AddrFamily::V4);
    EXPECT_EQ(reactor->send_to(stale, peer.addr, encode({})).error_or(0), EBADF);
    EXPECT_FALSE(reactor->is_quiescent(second.id));

    ASSERT_TRUE(peer.send(server, encode({.nonce = 5})));
    ASSERT_TRUE(pump_until(*reactor, [&] { return !second.got.empty(); }));
    EXPECT_TRUE(first.got.empty());
}

TEST_P(DatagramTest, AttachRefusesAStreamSocket) {
    Sink sink;
    os::UniqueFd tcp{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    ASSERT_TRUE(tcp);
    const auto id = reactor->attach_datagram(std::move(tcp), sink);
    ASSERT_FALSE(id);
    EXPECT_EQ(id.error(), EPROTOTYPE);
}

TEST_P(DatagramTest, Ipv6PeersKeepTheirAddress) {
    if (!ipv6_available()) {
        GTEST_SKIP() << "this host has no IPv6";
    }
    Sink sink;
    sink.after_datagram = [](Sink& s) {
        const Received& r = s.got.back();
        EXPECT_TRUE(s.reactor->send_to(s.id, r.from, r.payload));
    };
    const SocketAddr server = attach(sink, SocketAddr::loopback(AddrFamily::V6, 0));
    reactor->start_receiving_datagrams(sink.id);
    const Peer peer = Peer::bind(AddrFamily::V6);
    ASSERT_TRUE(peer.send(server, encode({.nonce = 6})));
    ASSERT_TRUE(pump_until(*reactor, [&] { return !sink.got.empty(); }));
    EXPECT_EQ(sink.got[0].from, peer.addr);
    EXPECT_EQ(sink.got[0].from.family, AddrFamily::V6);
    ASSERT_TRUE(pump_until(*reactor, [&] { return !peer.drain().empty(); }));
}

// An IPv4 peer of a dual-stack socket is reported, and answered, as IPv4.
TEST_P(DatagramTest, DualStackSocketTreatsIpv4PeersAsIpv4) {
    if (!ipv6_available()) {
        GTEST_SKIP() << "this host has no IPv6";
    }
    Sink sink;
    sink.after_datagram = [](Sink& s) {
        const Received& r = s.got.back();
        EXPECT_TRUE(s.reactor->send_to(s.id, r.from, r.payload));
    };
    const SocketAddr bound = attach(sink, SocketAddr::any(AddrFamily::V6, 0));
    reactor->start_receiving_datagrams(sink.id);
    const Peer peer = Peer::bind(AddrFamily::V4);
    ASSERT_TRUE(peer.send(SocketAddr::loopback(AddrFamily::V4, bound.port), encode({.nonce = 4})));
    ASSERT_TRUE(pump_until(*reactor, [&] { return !sink.got.empty(); }));
    EXPECT_EQ(sink.got[0].from, peer.addr);
    ASSERT_TRUE(pump_until(*reactor, [&] { return !peer.drain().empty(); }));
}

INSTANTIATE_TEST_SUITE_P(Reactors, DatagramTest,
                         ::testing::Values(ReactorKind::IoUring, ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
