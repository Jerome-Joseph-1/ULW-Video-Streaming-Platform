#include "net/reactor_factory.hpp"
#include "net/socket.hpp"
#include "os/system_clock.hpp"
#include "rt/room_router.hpp"

#include "memory_room_store.hpp"
#include "node_auth.hpp"
#include "support/fake_random.hpp"
#include "support/reactor_harness.hpp"
#include "wire.hpp"

#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using net::ReactorKind;
using rt::RouteError;
using ulw::test::pump_until;
namespace wire = rt::wire;

struct Received {
    std::uint64_t seq;
    std::string sender;
    std::string body;
    friend bool operator==(const Received&, const Received&) = default;
};

class Member final : public rt::IMember {
public:
    void deliver(const rt::Message& m) noexcept override {
        got.push_back({.seq = m.seq,
                       .sender = std::string(m.sender.view()),
                       .body = ulw::test::as_text(m.body)});
    }
    std::vector<Received> got;
};

class Events final : public rt::IRouterEvents {
public:
    void on_fenced_out(const core::RoomId& room, std::uint64_t generation,
                       rt::OwnerWrite write) noexcept override {
        fenced.push_back({room, generation, write});
    }
    void on_took_room(const core::RoomId& /*room*/, std::uint64_t generation) noexcept override {
        took.push_back(generation);
    }
    void on_peer_lost(const core::NodeId& peer) noexcept override {
        lost.emplace_back(peer.view());
    }
    void on_peer_refused(std::string_view why) noexcept override { refused.emplace_back(why); }

    struct Fence {
        core::RoomId room;
        std::uint64_t generation;
        rt::OwnerWrite write;
    };
    std::vector<Fence> fenced;
    std::vector<std::uint64_t> took;
    std::vector<std::string> lost;
    std::vector<std::string> refused;
};

// A test value, made up for these tests; real deployments take theirs from the environment.
constexpr std::string_view kSecret = "unit-test-node-secret-000000000000000";

struct Node {
    std::unique_ptr<ulw::test::MemoryRoomStore> store;
    Events events;
    std::unique_ptr<rt::RoomRouter> router;
    std::uint16_t port = 0;
};

// A process on the node-channel port that speaks the frames but may not hold the secret.
class RawPeer {
public:
    RawPeer(net::IReactor& reactor, std::uint16_t port)
        : reactor_(reactor), fd_(ulw::test::connect_loopback(port)) {}

    void send(const std::vector<std::byte>& bytes) {
        sent_.insert(sent_.end(), bytes.begin(), bytes.end());
        ASSERT_EQ(ulw::test::write_some(fd_.get(), bytes), bytes.size());
    }

    // Everything sent so far, for a replay.
    [[nodiscard]] const std::vector<std::byte>& sent() const noexcept { return sent_; }

    // The next frame from the node, or nullopt once it has closed the connection.
    std::optional<wire::Frame> next() {
        std::optional<wire::Frame> frame;
        const bool got = ulw::test::pump_until(reactor_, [&] {
            if (auto decoded = decoder_.next(); decoded && *decoded) {
                frame = **decoded;
                return true;
            }
            std::array<std::byte, 4096> buf{};
            const ssize_t n = ::recv(fd_.get(), buf.data(), buf.size(), MSG_DONTWAIT);
            if (n == 0 || (n < 0 && errno != EAGAIN)) {
                closed_ = true;
                return true;
            }
            if (n > 0) {
                decoder_.feed(std::span{buf}.first(static_cast<std::size_t>(n)));
            }
            return false;
        });
        EXPECT_TRUE(got) << "the node neither answered nor hung up";
        return frame;
    }

    // Reads until the node hangs up; false if it answers anything first.
    bool hung_up() { return !next() && closed_; }

private:
    net::IReactor& reactor_;
    os::UniqueFd fd_;
    wire::Decoder decoder_;
    std::vector<std::byte> sent_;
    bool closed_ = false;
};

class RoomRouterTest : public ::testing::TestWithParam<ReactorKind> {
protected:
    void SetUp() override {
        auto r = net::make_reactor(GetParam(), clock_, 1024);
        ASSERT_TRUE(r);
        reactor_ = std::move(*r);
    }

    void TearDown() override {
        for (auto& node : nodes_) {
            node->store->unwatch();
        }
        for (auto& node : nodes_) {
            node->store.reset();
            node->router.reset();
        }
    }

    Node& start(std::string_view name, std::string_view secret = kSecret) {
        auto node = std::make_unique<Node>();
        node->store = std::make_unique<ulw::test::MemoryRoomStore>(*reactor_, db_);
        auto listener = net::listen_tcp({.port = 0, .loopback_only = true});
        EXPECT_TRUE(listener);
        node->port = *net::local_port(listener->get());
        node->router = std::make_unique<rt::RoomRouter>(
            *reactor_, *node->store, clock_, random_,
            rt::RouterConfig{.self = *core::NodeId::parse(name),
                             .advertise = "127.0.0.1:" + std::to_string(node->port),
                             .secret = std::string(secret)},
            node->events);
        EXPECT_TRUE(node->router->start(std::move(*listener)));
        Node& out = *node;
        nodes_.push_back(std::move(node));
        EXPECT_TRUE(pump([&] { return out.router->healthy(); }));
        return out;
    }

    template <class Pred> bool pump(Pred pred) {
        return pump_until(*reactor_, [&] {
            for (auto& node : nodes_) {
                node->router->reap();
            }
            return pred();
        });
    }

    std::expected<void, RouteError> join(Node& node, Member& member) {
        std::optional<std::expected<void, RouteError>> result;
        node.router->join(room_, member, [&](auto r) noexcept { result = r; });
        if (!pump([&] { return result.has_value(); })) {
            ADD_FAILURE() << "join never answered";
            return std::unexpected(RouteError::Unavailable);
        }
        return *result;
    }

    std::expected<std::uint64_t, RouteError> send(Node& node, Member& member, std::string_view who,
                                                  std::string_view text) {
        std::optional<std::expected<std::uint64_t, RouteError>> result;
        const auto body = std::as_bytes(std::span{text});
        node.router->send(room_, member, *core::UserId::parse(who), {body.begin(), body.end()},
                          [&](auto r) noexcept { result = r; });
        if (!pump([&] { return result.has_value(); })) {
            ADD_FAILURE() << "send never answered";
            return std::unexpected(RouteError::Unavailable);
        }
        return *result;
    }

    // Sends again while the node is still finding the room's new owner.
    std::expected<std::uint64_t, RouteError>
    send_once_routed(Node& node, Member& member, std::string_view who, std::string_view text) {
        std::expected<std::uint64_t, RouteError> result = std::unexpected(RouteError::Unavailable);
        pump([&] {
            result = send(node, member, who, text);
            return result || result.error() != RouteError::Unavailable;
        });
        return result;
    }

    os::SystemClock clock_;
    ulw::test::FakeRandom random_;
    std::unique_ptr<net::IReactor> reactor_;
    ulw::test::MemoryRooms db_;
    std::vector<std::unique_ptr<Node>> nodes_;
    const core::RoomId room_ = core::RoomId::generate(clock_, random_);
};

TEST_P(RoomRouterTest, MembersOnDifferentNodesSeeEachOthersMessagesInOneOrder) {
    Node& a = start("chat-a");
    Node& b = start("chat-b");
    Member alice;
    Member bob;
    ASSERT_TRUE(join(a, alice));
    ASSERT_TRUE(join(b, bob));
    EXPECT_EQ(db_.rooms.at(room_).owner, *core::NodeId::parse("chat-a"));

    EXPECT_EQ(send(b, bob, "bob", "one"), 1U);
    EXPECT_EQ(send(a, alice, "alice", "two"), 2U);
    const std::vector<Received> expected{{.seq = 1, .sender = "bob", .body = "one"},
                                         {.seq = 2, .sender = "alice", .body = "two"}};
    ASSERT_TRUE(pump([&] { return bob.got.size() == 2; }));
    EXPECT_EQ(alice.got, expected);
    EXPECT_EQ(bob.got, expected);
    EXPECT_EQ(a.router->rooms_owned(), 1U);
    EXPECT_EQ(b.router->rooms_owned(), 0U);
    EXPECT_EQ(b.router->counters().forwarded, 1U);
}

TEST_P(RoomRouterTest, AWriteUnderAGenerationThatMovedOnIsFencedAndDeliveredNowhere) {
    Node& a = start("chat-a");
    Node& b = start("chat-b");
    Member alice;
    Member bob;
    ASSERT_TRUE(join(a, alice));
    ASSERT_TRUE(join(b, bob));
    ASSERT_EQ(send(a, alice, "alice", "before"), 1U);
    ASSERT_TRUE(pump([&] { return bob.got.size() == 1; }));

    // chat-a stops reaching the database, as a paused process would, and chat-b takes the room.
    a.store->reachable = false;
    db_.make_stale(room_);
    ASSERT_TRUE(pump([&] { return db_.rooms.at(room_).owner == *core::NodeId::parse("chat-b"); }));
    ASSERT_EQ(db_.rooms.at(room_).generation, 2U);
    a.store->reachable = true;

    EXPECT_EQ(send(a, alice, "alice", "stale"), std::unexpected(RouteError::Fenced));
    ASSERT_EQ(db_.refused_appends.size(), 1U);
    EXPECT_EQ(db_.refused_appends[0].second, 1U);
    ASSERT_FALSE(a.events.fenced.empty());
    EXPECT_EQ(a.events.fenced[0].write, rt::OwnerWrite::Append);
    EXPECT_EQ(a.events.fenced[0].generation, 1U);
    EXPECT_EQ(db_.rooms.at(room_).last_seq, 1U);
    EXPECT_EQ(a.router->rooms_owned(), 0U);

    // Once chat-a has found the new owner, its members carry on through it.
    EXPECT_EQ(send_once_routed(a, alice, "alice", "after"), 2U);
    ASSERT_TRUE(pump([&] { return alice.got.size() == 2 && bob.got.size() == 2; }));
    const std::vector<Received> expected{{.seq = 1, .sender = "alice", .body = "before"},
                                         {.seq = 2, .sender = "alice", .body = "after"}};
    EXPECT_EQ(alice.got, expected);
    EXPECT_EQ(bob.got, expected);
}

TEST_P(RoomRouterTest, AnOwnerThatLetsARoomGoTellsItsSubscribersWhoMissedTheNotice) {
    Node& a = start("chat-a");
    Node& b = start("chat-b");
    Node& c = start("chat-c");
    Member alice;
    Member bob;
    Member carol;
    ASSERT_TRUE(join(a, alice));
    ASSERT_TRUE(join(b, bob));
    // chat-b hears of no takeover from here on.
    b.store->deaf = true;
    a.store->reachable = false;
    db_.make_stale(room_);
    ASSERT_TRUE(join(c, carol));
    ASSERT_EQ(db_.rooms.at(room_).owner, *core::NodeId::parse("chat-c"));
    a.store->reachable = true;

    // chat-b still routes to chat-a, whose write is fenced; chat-a then tells chat-b, which
    // finds chat-c well before any periodic lookup would.
    EXPECT_EQ(send(b, bob, "bob", "to the old owner"), std::unexpected(RouteError::Fenced));
    std::expected<std::uint64_t, RouteError> sent = std::unexpected(RouteError::Unavailable);
    ASSERT_TRUE(pump_until(
        *reactor_,
        [&] {
            sent = send(b, bob, "bob", "to the new owner");
            return sent.has_value();
        },
        std::chrono::seconds(5)));
    ASSERT_TRUE(pump([&] { return !carol.got.empty() && !bob.got.empty(); }));
    EXPECT_EQ(carol.got.back().body, "to the new owner");
    EXPECT_EQ(bob.got.back().body, "to the new owner");
}

TEST_P(RoomRouterTest, SendingToARoomTheMemberHasNotJoinedIsRefused) {
    Node& a = start("chat-a");
    Member alice;
    Member mallory;
    ASSERT_TRUE(join(a, alice));
    EXPECT_EQ(send(a, mallory, "mallory", "hi"), std::unexpected(RouteError::NotJoined));
    EXPECT_EQ(db_.rooms.at(room_).last_seq, 0U);
}

TEST_P(RoomRouterTest, AMemberWhoLeftReceivesNothingMore) {
    Node& a = start("chat-a");
    Node& b = start("chat-b");
    Member alice;
    Member bob;
    Member carol;
    ASSERT_TRUE(join(a, alice));
    ASSERT_TRUE(join(b, bob));
    ASSERT_TRUE(join(b, carol));
    b.router->leave(room_, carol);
    ASSERT_EQ(send(a, alice, "alice", "hi"), 1U);
    ASSERT_TRUE(pump([&] { return bob.got.size() == 1; }));
    EXPECT_TRUE(carol.got.empty());
}

TEST_P(RoomRouterTest, AJoinFailsWhenTheOwnerCannotBeReachedAndThePeerIsReportedLost) {
    Node& a = start("chat-a");
    Member alice;
    ASSERT_TRUE(join(a, alice));
    Node& b = start("chat-b");
    // Where chat-a says it listens, nothing does.
    std::uint16_t dead = 0;
    {
        auto probe = net::listen_tcp({.port = 0, .loopback_only = true});
        ASSERT_TRUE(probe);
        dead = *net::local_port(probe->get());
    }
    db_.addresses["chat-a"] = "127.0.0.1:" + std::to_string(dead);
    Member bob;
    EXPECT_EQ(join(b, bob), std::unexpected(RouteError::Unavailable));
    EXPECT_EQ(b.events.lost, std::vector<std::string>{"chat-a"});
    EXPECT_EQ(b.router->rooms_joined(), 0U);
}

TEST_P(RoomRouterTest, RoomsReleasedByADrainAreTakenAtOnceByANodeWithMembers) {
    Node& a = start("chat-a");
    Node& b = start("chat-b");
    Member alice;
    Member bob;
    ASSERT_TRUE(join(a, alice));
    ASSERT_TRUE(join(b, bob));
    bool released = false;
    a.router->release_rooms([&](rt::StoreResult<void> r) noexcept { released = r.has_value(); });
    ASSERT_TRUE(pump([&] { return released; }));
    ASSERT_TRUE(pump([&] { return db_.rooms.at(room_).owner == *core::NodeId::parse("chat-b"); }));
    EXPECT_EQ(b.events.took, std::vector<std::uint64_t>{2});
    EXPECT_EQ(send(b, bob, "bob", "still here"), 1U);
}

// ---- the node channel's handshake (ADR-0037)

TEST_P(RoomRouterTest, APeerThatSkipsTheHandshakeIsCutOffBeforeAnythingHappens) {
    const Node& a = start("chat-a");
    RawPeer peer(*reactor_, a.port);
    std::vector<std::byte> frames;
    wire::encode_subscribe(frames, 1, room_);
    const auto body = std::as_bytes(std::span{std::string_view{"forged"}});
    wire::encode_send(frames, 2, room_, *core::UserId::parse("alice"), body);
    peer.send(frames);
    EXPECT_TRUE(peer.hung_up());
    // No room was made for it, and nothing was sequenced.
    EXPECT_TRUE(db_.rooms.empty());
    EXPECT_EQ(a.events.refused, std::vector<std::string>{"no hello"});
}

TEST_P(RoomRouterTest, AHelloIsNotEnoughWithoutTheProofThatFollowsIt) {
    const Node& a = start("chat-a");
    RawPeer peer(*reactor_, a.port);
    std::vector<std::byte> frames;
    wire::encode_hello(frames, *core::NodeId::parse("chat-x"), wire::Nonce{});
    wire::encode_subscribe(frames, 1, room_);
    peer.send(frames);
    const auto challenge = peer.next();
    ASSERT_TRUE(challenge && std::holds_alternative<wire::Challenge>(*challenge));
    EXPECT_TRUE(peer.hung_up());
    EXPECT_TRUE(db_.rooms.empty());
    EXPECT_EQ(a.events.refused, std::vector<std::string>{"no proof"});
}

TEST_P(RoomRouterTest, NodesWithDifferentSecretsNeverTalk) {
    Node& a = start("chat-a", "first-secret-0000000000000000000000000");
    Node& b = start("chat-b", "second-secret-000000000000000000000000");
    Member alice;
    Member bob;
    ASSERT_TRUE(join(a, alice));
    EXPECT_EQ(join(b, bob), std::unexpected(RouteError::Unavailable));
    // chat-b checks chat-a's tag first, and never proves itself to a node that failed.
    EXPECT_EQ(b.events.refused, std::vector<std::string>{"bad challenge"});
    EXPECT_TRUE(a.events.refused.empty());
    EXPECT_EQ(db_.rooms.at(room_).last_seq, 0U);
}

TEST_P(RoomRouterTest, ARecordedHandshakeReplaysToNothing) {
    Node& a = start("chat-a");
    Member alice;
    ASSERT_TRUE(join(a, alice));
    const core::NodeId x = *core::NodeId::parse("chat-x");
    const auto secret = std::as_bytes(std::span{kSecret});

    // A genuine handshake with the secret works, and its subscription is taken.
    RawPeer genuine(*reactor_, a.port);
    wire::Nonce mine{};
    random_.fill(mine);
    std::vector<std::byte> hello;
    wire::encode_hello(hello, x, mine);
    genuine.send(hello);
    const auto challenge = genuine.next();
    ASSERT_TRUE(challenge);
    const auto& c = std::get<wire::Challenge>(*challenge);
    const auto expected =
        rt::auth::acceptor_tag(secret, x, *core::NodeId::parse("chat-a"), mine, c.nonce);
    ASSERT_TRUE(expected && rt::auth::same_tag(*expected, c.mac));
    std::vector<std::byte> rest;
    wire::encode_proof(rest, *rt::auth::dialer_tag(secret, x, c.node, mine, c.nonce));
    wire::encode_subscribe(rest, 1, room_);
    genuine.send(rest);
    const auto reply = genuine.next();
    ASSERT_TRUE(reply);
    EXPECT_EQ(std::get<wire::Reply>(*reply).status, wire::Status::Ok);

    // The same bytes again: the node's fresh nonce makes the recorded proof worthless.
    RawPeer replay(*reactor_, a.port);
    replay.send(genuine.sent());
    const auto second = replay.next();
    ASSERT_TRUE(second && std::holds_alternative<wire::Challenge>(*second));
    EXPECT_TRUE(replay.hung_up());
    EXPECT_EQ(a.events.refused, std::vector<std::string>{"bad proof"});
}

INSTANTIATE_TEST_SUITE_P(Reactors, RoomRouterTest,
                         ::testing::Values(ReactorKind::IoUring, ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
