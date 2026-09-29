#include "net/reactor_factory.hpp"
#include "net/socket.hpp"
#include "os/system_clock.hpp"
#include "rt/room_router.hpp"

#include "memory_room_store.hpp"
#include "support/fake_random.hpp"
#include "support/reactor_harness.hpp"

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

    struct Fence {
        core::RoomId room;
        std::uint64_t generation;
        rt::OwnerWrite write;
    };
    std::vector<Fence> fenced;
    std::vector<std::uint64_t> took;
    std::vector<std::string> lost;
};

struct Node {
    std::unique_ptr<ulw::test::MemoryRoomStore> store;
    Events events;
    std::unique_ptr<rt::RoomRouter> router;
    std::uint16_t port = 0;
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

    Node& start(std::string_view name) {
        auto node = std::make_unique<Node>();
        node->store = std::make_unique<ulw::test::MemoryRoomStore>(*reactor_, db_);
        auto listener = net::listen_tcp({.port = 0, .loopback_only = true});
        EXPECT_TRUE(listener);
        node->port = *net::local_port(listener->get());
        node->router = std::make_unique<rt::RoomRouter>(
            *reactor_, *node->store, clock_,
            rt::RouterConfig{.self = *core::NodeId::parse(name),
                             .advertise = "127.0.0.1:" + std::to_string(node->port)},
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

INSTANTIATE_TEST_SUITE_P(Reactors, RoomRouterTest,
                         ::testing::Values(ReactorKind::IoUring, ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
