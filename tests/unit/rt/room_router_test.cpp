#include "net/reactor_factory.hpp"
#include "net/socket.hpp"
#include "os/system_clock.hpp"
#include "rt/room_router.hpp"

#include "memory_room_store.hpp"
#include "node_auth.hpp"
#include "support/fake_random.hpp"
#include "support/reactor_harness.hpp"
#include "wire.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <fcntl.h>
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
    void on_node_taken() noexcept override { ++taken; }

    struct Fence {
        core::RoomId room;
        std::uint64_t generation;
        rt::OwnerWrite write;
    };
    std::vector<Fence> fenced;
    std::vector<std::uint64_t> took;
    std::vector<std::string> lost;
    std::vector<std::string> refused;
    int taken = 0;
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
    // A receive buffer as small as the kernel allows makes the node's queue fill first.
    RawPeer(net::IReactor& reactor, std::uint16_t port, bool tiny_window = false)
        : reactor_(reactor), fd_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
        if (tiny_window) {
            const int bytes = 4096;
            ::setsockopt(fd_.get(), SOL_SOCKET, SO_RCVBUF, &bytes, sizeof bytes);
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        // connect() takes every address family through the generic sockaddr header.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        EXPECT_EQ(::connect(fd_.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr), 0);
        ::fcntl(fd_.get(), F_SETFL, ::fcntl(fd_.get(), F_GETFL) | O_NONBLOCK);
    }

    // The dialer's side of the handshake, done right; true once the node has proven itself.
    bool authenticate(std::string_view secret, const core::NodeId& self, const core::NodeId& node,
                      core::ports::IRandom& random) {
        const auto key = std::as_bytes(std::span{secret});
        wire::Nonce mine{};
        random.fill(mine);
        std::vector<std::byte> hello;
        wire::encode_hello(hello, self, mine);
        send(hello);
        const auto challenge = next();
        const auto* c = challenge ? std::get_if<wire::Challenge>(&*challenge) : nullptr;
        if (c == nullptr) {
            return false;
        }
        const auto expected = rt::auth::acceptor_tag(key, self, node, mine, c->nonce);
        if (!expected || !rt::auth::same_tag(*expected, c->mac)) {
            return false;
        }
        std::vector<std::byte> proof;
        wire::encode_proof(proof, *rt::auth::dialer_tag(key, self, node, mine, c->nonce));
        send(proof);
        return true;
    }

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

    // A connection the test accepted, as the node's peer at the other end.
    RawPeer(net::IReactor& reactor, os::UniqueFd accepted)
        : reactor_(reactor), fd_(std::move(accepted)) {}

private:
    net::IReactor& reactor_;
    os::UniqueFd fd_;
    wire::Decoder decoder_;
    std::vector<std::byte> sent_;
    bool closed_ = false;
};

// Listens where a node is advertised, to play that node: the router under test dials it.
class RawOwner {
public:
    explicit RawOwner(net::IReactor& reactor) : reactor_(reactor) {
        fd_ = os::UniqueFd{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
        // Inherited by the accepted socket, so a peer that stops reading fills up quickly.
        const int bytes = 4096;
        ::setsockopt(fd_.get(), SOL_SOCKET, SO_RCVBUF, &bytes, sizeof bytes);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        // bind() takes every address family through the generic sockaddr header.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        EXPECT_EQ(::bind(fd_.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr), 0);
        EXPECT_EQ(::listen(fd_.get(), 8), 0);
        port_ = *net::local_port(fd_.get());
    }

    [[nodiscard]] std::string address() const { return "127.0.0.1:" + std::to_string(port_); }

    // The router's connection, once it has dialled.
    std::unique_ptr<RawPeer> accept() {
        os::UniqueFd conn;
        EXPECT_TRUE(ulw::test::pump_until(reactor_, [&] {
            conn =
                os::UniqueFd{::accept4(fd_.get(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK)};
            return static_cast<bool>(conn);
        }));
        if (!conn) {
            return nullptr;
        }
        return std::make_unique<RawPeer>(reactor_, std::move(conn));
    }

private:
    net::IReactor& reactor_;
    os::UniqueFd fd_;
    std::uint16_t port_ = 0;
};

// Limits other than the router's own defaults.
struct Tuning {
    std::optional<core::Millis> idle_release = std::nullopt;
    std::optional<std::size_t> max_rooms = std::nullopt;
    std::optional<core::Millis> revalidate_every = std::nullopt;
    // The store refuses to record the node, which then never becomes ready.
    bool refuse_advertise = false;
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

    Node& start(std::string_view name, std::string_view secret = kSecret, Tuning tuning = {}) {
        auto node = std::make_unique<Node>();
        node->store = std::make_unique<ulw::test::MemoryRoomStore>(*reactor_, db_);
        node->store->refuse_advertise = tuning.refuse_advertise;
        auto listener = net::listen_tcp({.port = 0, .loopback_only = true});
        EXPECT_TRUE(listener);
        node->port = *net::local_port(listener->get());
        rt::RouterConfig config{.self = *core::NodeId::parse(name),
                                .advertise = "127.0.0.1:" + std::to_string(node->port),
                                .secret = std::string(secret)};
        config.idle_release = tuning.idle_release.value_or(config.idle_release);
        config.max_rooms = tuning.max_rooms.value_or(config.max_rooms);
        config.revalidate_every = tuning.revalidate_every.value_or(config.revalidate_every);
        node->router = std::make_unique<rt::RoomRouter>(*reactor_, *node->store, clock_, random_,
                                                        std::move(config), node->events);
        EXPECT_TRUE(node->router->start(std::move(*listener)));
        Node& out = *node;
        nodes_.push_back(std::move(node));
        if (!tuning.refuse_advertise) {
            EXPECT_TRUE(pump([&] { return out.router->healthy() || out.events.taken > 0; }));
        }
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

    // Each call a message of its own, unless it names the key of an earlier one.
    rt::MessageKey next_key() { return *rt::MessageKey::parse("m" + std::to_string(++keys_)); }

    std::expected<std::uint64_t, RouteError> send(Node& node, Member& member, std::string_view who,
                                                  std::string_view text) {
        return send(node, member, who, text, next_key());
    }

    std::expected<std::uint64_t, RouteError> send(Node& node, Member& member, std::string_view who,
                                                  std::string_view text,
                                                  const rt::MessageKey& key) {
        std::optional<std::expected<std::uint64_t, RouteError>> result;
        const auto body = std::as_bytes(std::span{text});
        node.router->send(room_, member, *core::UserId::parse(who), key, {body.begin(), body.end()},
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
    int keys_ = 0;
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

TEST_P(RoomRouterTest, ANodeThatMissedATakeoverFindsTheNewOwnerByReadingOwnersNotResolving) {
    Node& a = start("chat-a");
    Node& b = start("chat-b", kSecret, {.revalidate_every = core::Millis{300}});
    Node& c = start("chat-c");
    Member alice;
    Member bob;
    Member carol;
    ASSERT_TRUE(join(a, alice));
    ASSERT_TRUE(join(b, bob));
    // chat-b hears of no takeover, and chat-a, cut off from the database for the rest of the
    // test, never learns it was fenced and so never tells chat-b either.
    b.store->deaf = true;
    a.store->reachable = false;
    db_.make_stale(room_);
    ASSERT_TRUE(join(c, carol));
    ASSERT_EQ(db_.rooms.at(room_).owner, *core::NodeId::parse("chat-c"));
    const std::size_t resolves = b.store->resolves;
    const std::size_t reads = b.store->owner_reads;

    // Only revalidation can move chat-b over; carol's messages reach bob once it has.
    // One message per revalidation period is plenty; each send pumps until it is answered.
    auto next_send = std::chrono::steady_clock::now();
    int sent = 0;
    ASSERT_TRUE(pump([&] {
        if (std::chrono::steady_clock::now() >= next_send) {
            next_send += std::chrono::milliseconds(300);
            sent += send(c, carol, "carol", "anyone there") ? 1 : 0;
        }
        return !bob.got.empty();
    }));
    EXPECT_EQ(bob.got.back().sender, "carol");
    EXPECT_GT(sent, 0);
    EXPECT_GT(b.store->owner_reads, reads);
    EXPECT_EQ(b.store->resolves, resolves);
}

TEST_P(RoomRouterTest, ASendRepeatedWithItsKeyGetsItsFirstSeqAndIsDeliveredOnce) {
    Node& a = start("chat-a");
    Node& b = start("chat-b");
    Member alice;
    Member bob;
    ASSERT_TRUE(join(a, alice));
    ASSERT_TRUE(join(b, bob));
    const rt::MessageKey key = next_key();
    ASSERT_EQ(send(b, bob, "bob", "hello", key), 1U);
    ASSERT_TRUE(pump([&] { return bob.got.size() == 1; }));

    // chat-b delivered it, so it answers without asking the owner.
    EXPECT_EQ(send(b, bob, "bob", "hello", key), 1U);
    EXPECT_EQ(b.router->counters().duplicates, 1U);
    EXPECT_EQ(b.router->counters().forwarded, 1U);
    // A key is the sender's own: another sender's message under it is a message of its own.
    EXPECT_EQ(send(a, alice, "alice", "hi", key), 2U);
    ASSERT_TRUE(pump([&] { return bob.got.size() == 2; }));
    ulw::test::pump_pending(*reactor_);
    const std::vector<Received> expected{{.seq = 1, .sender = "bob", .body = "hello"},
                                         {.seq = 2, .sender = "alice", .body = "hi"}};
    EXPECT_EQ(alice.got, expected);
    EXPECT_EQ(bob.got, expected);
    EXPECT_EQ(db_.rooms.at(room_).last_seq, 2U);
}

TEST_P(RoomRouterTest, ARetryQueuedBehindItsFirstTryIsAnsweredByTheOwnerWithTheFirstSeq) {
    Node& a = start("chat-a");
    Node& b = start("chat-b");
    Member alice;
    Member bob;
    ASSERT_TRUE(join(a, alice));
    ASSERT_TRUE(join(b, bob));
    // Both tries reach the owner before the first is sequenced, so chat-b cannot know.
    a.store->hold = true;
    const rt::MessageKey key = next_key();
    const auto body = std::as_bytes(std::span{std::string_view{"once"}});
    std::vector<std::expected<std::uint64_t, RouteError>> answers;
    for (int attempt = 0; attempt < 2; ++attempt) {
        b.router->send(room_, bob, *core::UserId::parse("bob"), key, {body.begin(), body.end()},
                       [&](auto r) noexcept { answers.push_back(r); });
    }
    // The owner's only unnamed store call here is the first try's append.
    ASSERT_TRUE(pump([&] { return a.store->waiting("") == 1; }));
    ulw::test::pump_pending(*reactor_);
    a.store->release_held();
    ASSERT_TRUE(pump([&] { return answers.size() == 2 && bob.got.size() == 1; }));
    EXPECT_EQ(answers[0], 1U);
    EXPECT_EQ(answers[1], 1U);
    ulw::test::pump_pending(*reactor_);
    EXPECT_EQ(alice.got.size(), 1U);
    EXPECT_EQ(bob.got.size(), 1U);
    EXPECT_EQ(db_.rooms.at(room_).last_seq, 1U);
    EXPECT_EQ(a.router->counters().duplicates, 1U);
}

TEST_P(RoomRouterTest, ANodeThatTakesTheRoomOverKnowsTheKeysItDeliveredAsAMember) {
    Node& a = start("chat-a");
    Node& b = start("chat-b");
    Member alice;
    Member bob;
    ASSERT_TRUE(join(a, alice));
    ASSERT_TRUE(join(b, bob));
    const rt::MessageKey key = next_key();
    ASSERT_EQ(send(a, alice, "alice", "before the takeover", key), 1U);
    ASSERT_TRUE(pump([&] { return bob.got.size() == 1; }));

    a.store->reachable = false;
    db_.make_stale(room_);
    ASSERT_TRUE(pump([&] { return b.router->rooms_owned() == 1; }));
    // alice, having heard nothing from chat-a, tries again through chat-b: the new owner.
    Member alice_again;
    ASSERT_TRUE(join(b, alice_again));
    EXPECT_EQ(send(b, alice_again, "alice", "before the takeover", key), 1U);
    EXPECT_EQ(db_.rooms.at(room_).last_seq, 1U);
    EXPECT_EQ(send(b, alice_again, "alice", "after the takeover"), 2U);
}

TEST_P(RoomRouterTest, AMemberThatJoinsTwiceBeforeTheFirstIsAnsweredHearsEachMessageOnce) {
    Node& a = start("chat-a");
    Member alice;
    int answers = 0;
    a.router->join(room_, alice, [&](auto r) noexcept { answers += r ? 1 : 0; });
    a.router->join(room_, alice, [&](auto r) noexcept { answers += r ? 1 : 0; });
    ASSERT_TRUE(pump([&] { return answers == 2; }));
    ASSERT_EQ(send(a, alice, "alice", "once"), 1U);
    ASSERT_TRUE(pump([&] { return !alice.got.empty(); }));
    ulw::test::pump_pending(*reactor_);
    EXPECT_EQ(alice.got.size(), 1U);
}

TEST_P(RoomRouterTest, AWriteInFlightWhenAHeartbeatIsFencedIsAnsweredAsUnknown) {
    Node& a = start("chat-a");
    Member alice;
    ASSERT_TRUE(join(a, alice));
    a.store->hold = true;
    std::optional<std::expected<std::uint64_t, RouteError>> result;
    const auto body = std::as_bytes(std::span{std::string_view{"maybe"}});
    a.router->send(room_, alice, *core::UserId::parse("alice"), next_key(),
                   {body.begin(), body.end()}, [&](auto r) noexcept { result = r; });
    // The append waits, and so does the next heartbeat.
    ASSERT_TRUE(pump([&] { return a.store->waiting("heartbeat") == 1; }));
    db_.take(room_, *core::NodeId::parse("chat-b"));
    // The heartbeat's answer comes first, as it may from another session of the pool.
    a.store->release_held("heartbeat");
    ASSERT_TRUE(pump([&] { return result.has_value(); }));
    EXPECT_EQ(*result, std::unexpected(RouteError::Unavailable));
    EXPECT_EQ(a.router->rooms_owned(), 0U);
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

// ---- the node channel's handshake (ADR-0035)

TEST_P(RoomRouterTest, APeerThatSkipsTheHandshakeIsCutOffBeforeAnythingHappens) {
    const Node& a = start("chat-a");
    RawPeer peer(*reactor_, a.port);
    std::vector<std::byte> frames;
    wire::encode_subscribe(frames, 1, room_);
    const auto body = std::as_bytes(std::span{std::string_view{"forged"}});
    wire::encode_send(frames, 2, room_, *core::UserId::parse("alice"), *rt::MessageKey::parse("k"),
                      body);
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

TEST_P(RoomRouterTest, ASubscriberThatStopsReadingIsCutOffInsteadOfQueuedForWithoutEnd) {
    Node& a = start("chat-a");
    Member alice;
    ASSERT_TRUE(join(a, alice));
    RawPeer stalled(*reactor_, a.port, true);
    ASSERT_TRUE(stalled.authenticate(kSecret, *core::NodeId::parse("chat-x"),
                                     *core::NodeId::parse("chat-a"), random_));
    std::vector<std::byte> subscribe;
    wire::encode_subscribe(subscribe, 1, room_);
    stalled.send(subscribe);
    const auto reply = stalled.next();
    ASSERT_TRUE(reply && std::holds_alternative<wire::Reply>(*reply));

    // Never read again. Each message fans out one delivery of 60 KiB to it.
    const std::string body(std::size_t{60} * 1024, 'm');
    for (int i = 0; i < 400 && a.router->counters().slow_peers == 0; ++i) {
        ASSERT_TRUE(send(a, alice, "alice", body));
    }
    EXPECT_EQ(a.router->counters().slow_peers, 1U);
    // Its own members are still served.
    EXPECT_TRUE(send(a, alice, "alice", "still here"));
}

TEST_P(RoomRouterTest, ARoomNobodyUsesAnyMoreIsGivenUpForOthersToTake) {
    Node& a = start("chat-a", kSecret, {.idle_release = core::Millis{0}, .max_rooms = {}});
    Member alice;
    ASSERT_TRUE(join(a, alice));
    ASSERT_EQ(a.router->rooms_owned(), 1U);
    a.router->leave(room_, alice);
    ASSERT_TRUE(pump([&] { return a.router->rooms_owned() == 0; }));
    // Claimable at once, not after the owner's heartbeat goes stale.
    EXPECT_TRUE(pump([&] { return db_.rooms.at(room_).stale; }));

    // Used again, it is simply taken again.
    Node& b = start("chat-b");
    Member bob;
    ASSERT_TRUE(join(b, bob));
    EXPECT_EQ(db_.rooms.at(room_).owner, *core::NodeId::parse("chat-b"));
}

TEST_P(RoomRouterTest, ASubscriberWhoseConnectionClosedDoesNotKeepARoomAlive) {
    Node& a = start("chat-a", kSecret, {.idle_release = core::Millis{0}, .max_rooms = {}});
    Member alice;
    ASSERT_TRUE(join(a, alice));
    {
        RawPeer peer(*reactor_, a.port);
        ASSERT_TRUE(peer.authenticate(kSecret, *core::NodeId::parse("chat-x"),
                                      *core::NodeId::parse("chat-a"), random_));
        std::vector<std::byte> subscribe;
        wire::encode_subscribe(subscribe, 1, room_);
        peer.send(subscribe);
        const auto reply = peer.next();
        ASSERT_TRUE(reply && std::holds_alternative<wire::Reply>(*reply));
    }
    a.router->leave(room_, alice);
    // Released after a beat or two; the closed subscriber must not hold it.
    EXPECT_TRUE(pump([&] { return a.router->rooms_owned() == 0; }));
}

TEST_P(RoomRouterTest, AJoinPastTheNodesRoomLimitIsBusy) {
    Node& a = start("chat-a", kSecret, {.idle_release = {}, .max_rooms = 1});
    Member alice;
    ASSERT_TRUE(join(a, alice));
    std::optional<std::expected<void, RouteError>> second;
    a.router->join(core::RoomId::generate(clock_, random_), alice,
                   [&](auto r) noexcept { second = r; });
    EXPECT_EQ(second, std::unexpected(RouteError::Busy));
    // The room already joined is not a new one.
    Member bob;
    EXPECT_TRUE(join(a, bob));
}

TEST_P(RoomRouterTest, ARoomsQueueIsBoundedInBytesNotInWrites) {
    Node& a = start("chat-a");
    Member alice;
    ASSERT_TRUE(join(a, alice));
    a.store->hold = true;
    // Sixteen writes of 60 KiB fit the room's 1 MiB; the seventeenth does not, though a count
    // of writes would have taken hundreds.
    const std::string big(std::size_t{60} * 1024, 'q');
    std::vector<std::optional<std::expected<std::uint64_t, RouteError>>> results(17);
    for (auto& result : results) {
        const auto body = std::as_bytes(std::span{big});
        a.router->send(room_, alice, *core::UserId::parse("alice"), next_key(),
                       {body.begin(), body.end()}, [&result](auto r) noexcept { result = r; });
    }
    EXPECT_EQ(results.back(), std::unexpected(RouteError::Busy));
    a.store->release_held();
    ASSERT_TRUE(pump([&] { return results[15].has_value(); }));
    for (std::size_t i = 0; i < 16; ++i) {
        EXPECT_EQ(results[i], i + 1) << i;
    }
}

// Plays chat-a, the room's recorded owner, for `dialler`: answers the handshake as `as` and
// takes the first Subscribe. Returns the connection, with the Subscribe answered.
std::unique_ptr<RawPeer> play_owner(RawOwner& owner, const core::NodeId& dialler,
                                    const core::NodeId& as, core::ports::IRandom& random) {
    auto conn = owner.accept();
    if (!conn) {
        return nullptr;
    }
    const auto hello = conn->next();
    const auto* h = hello ? std::get_if<wire::Hello>(&*hello) : nullptr;
    EXPECT_NE(h, nullptr);
    if (h == nullptr) {
        return nullptr;
    }
    wire::Nonce mine{};
    random.fill(mine);
    const auto key = std::as_bytes(std::span{kSecret});
    std::vector<std::byte> challenge;
    wire::encode_challenge(challenge, as, mine,
                           *rt::auth::acceptor_tag(key, dialler, as, h->nonce, mine));
    conn->send(challenge);
    return conn;
}

TEST_P(RoomRouterTest, ALinkBrokenInsideASendIsTakenDownAfterwardsNotInsideIt) {
    RawOwner owner(*reactor_);
    db_.addresses["chat-a"] = owner.address();
    db_.rooms.emplace(room_, ulw::test::MemoryRooms::Room{.owner = *core::NodeId::parse("chat-a")});
    Node& b = start("chat-b");
    Member bob;
    std::optional<std::expected<void, RouteError>> joined;
    b.router->join(room_, bob, [&](auto r) noexcept { joined = r; });
    auto conn =
        play_owner(owner, *core::NodeId::parse("chat-b"), *core::NodeId::parse("chat-a"), random_);
    ASSERT_TRUE(conn);
    std::optional<wire::Frame> subscribe = conn->next();
    while (subscribe && !std::holds_alternative<wire::Subscribe>(*subscribe)) {
        subscribe = conn->next();
    }
    ASSERT_TRUE(subscribe);
    std::vector<std::byte> ok;
    wire::encode_reply(ok, std::get<wire::Subscribe>(*subscribe).request, wire::Status::Ok, 0);
    conn->send(ok);
    ASSERT_TRUE(pump([&] { return joined.has_value(); }));
    ASSERT_TRUE(*joined);

    // chat-a stops reading; bob's forwards pile up on the link until it is found broken.
    const std::string big(std::size_t{60} * 1024, 'f');
    int answered = 0;
    for (int i = 0; i < 400; ++i) {
        const auto body = std::as_bytes(std::span{big});
        b.router->send(room_, bob, *core::UserId::parse("bob"), next_key(),
                       {body.begin(), body.end()}, [&](auto) noexcept { ++answered; });
        if (b.router->counters().slow_peers > 0) {
            break;
        }
        ulw::test::pump_pending(*reactor_);
    }
    ASSERT_EQ(b.router->counters().slow_peers, 1U);
    // The send that found it did not take the link down, or fail anything, from inside itself.
    EXPECT_TRUE(b.events.lost.empty());
    EXPECT_EQ(answered, 0);
    ASSERT_TRUE(pump([&] { return !b.events.lost.empty(); }));
    EXPECT_EQ(b.events.lost, std::vector<std::string>{"chat-a"});
    EXPECT_TRUE(pump([&] { return answered > 0; }));
}

// Both sides at once, since each waits out the same five seconds.
TEST_P(RoomRouterTest, AHandshakeNobodyFinishesIsDroppedAfterFiveSecondsOnEitherSide) {
    RawOwner owner(*reactor_);
    db_.addresses["chat-a"] = owner.address();
    db_.rooms.emplace(room_, ulw::test::MemoryRooms::Room{.owner = *core::NodeId::parse("chat-a")});
    Node& b = start("chat-b");

    // Dialled by chat-b, "chat-a" reads the Hello and never answers it.
    Member bob;
    std::optional<std::expected<void, RouteError>> joined;
    b.router->join(room_, bob, [&](auto r) noexcept { joined = r; });
    auto dialled = owner.accept();
    ASSERT_TRUE(dialled);
    const auto hello = dialled->next();
    ASSERT_TRUE(hello && std::holds_alternative<wire::Hello>(*hello));
    // Dialling chat-b, a peer connects and says nothing.
    RawPeer silent(*reactor_, b.port);
    const auto began = std::chrono::steady_clock::now();

    EXPECT_TRUE(silent.hung_up());
    ASSERT_TRUE(pump([&] { return !b.events.lost.empty() && joined.has_value(); }));
    EXPECT_GE(std::chrono::steady_clock::now() - began, std::chrono::seconds(5));
    EXPECT_EQ(b.events.lost, std::vector<std::string>{"chat-a"});
    EXPECT_EQ(*joined, std::unexpected(RouteError::Unavailable));
    EXPECT_TRUE(dialled->hung_up());
}

TEST_P(RoomRouterTest, AChallengeTagReflectedBackAsTheProofIsRefused) {
    const Node& a = start("chat-a");
    RawPeer peer(*reactor_, a.port);
    wire::Nonce mine{};
    random_.fill(mine);
    std::vector<std::byte> hello;
    wire::encode_hello(hello, *core::NodeId::parse("chat-x"), mine);
    peer.send(hello);
    const auto challenge = peer.next();
    ASSERT_TRUE(challenge && std::holds_alternative<wire::Challenge>(*challenge));
    // Without the secret, the only tag to hand is the node's own.
    std::vector<std::byte> rest;
    wire::encode_proof(rest, std::get<wire::Challenge>(*challenge).mac);
    wire::encode_subscribe(rest, 1, room_);
    peer.send(rest);
    EXPECT_TRUE(peer.hung_up());
    EXPECT_TRUE(db_.rooms.empty());
    EXPECT_EQ(a.events.refused, std::vector<std::string>{"bad proof"});
}

TEST_P(RoomRouterTest, ADialerAnsweredByANodeOtherThanTheOneItDialledGoesNoFurther) {
    RawOwner owner(*reactor_);
    db_.addresses["chat-a"] = owner.address();
    db_.rooms.emplace(room_, ulw::test::MemoryRooms::Room{.owner = *core::NodeId::parse("chat-a")});
    Node& b = start("chat-b");
    Member bob;
    std::optional<std::expected<void, RouteError>> joined;
    b.router->join(room_, bob, [&](auto r) noexcept { joined = r; });
    // A genuine node, with the secret, but not the one chat-b means to reach: its tag is valid
    // for the node it names.
    auto conn =
        play_owner(owner, *core::NodeId::parse("chat-b"), *core::NodeId::parse("chat-z"), random_);
    ASSERT_TRUE(conn);
    EXPECT_TRUE(conn->hung_up());
    ASSERT_TRUE(pump([&] { return joined.has_value(); }));
    EXPECT_EQ(*joined, std::unexpected(RouteError::Unavailable));
    EXPECT_EQ(b.events.refused, std::vector<std::string>{"no challenge"});
}

TEST_P(RoomRouterTest, ASecondLiveProcessUnderANodesNameTakesNothing) {
    Node& first = start("chat-a");
    Member alice;
    ASSERT_TRUE(join(first, alice));
    Node& second = start("chat-a");
    EXPECT_EQ(second.events.taken, 1);
    EXPECT_FALSE(second.router->healthy());
    // It routes nothing and claims nothing; the first run's room is untouched.
    Member mallory;
    std::optional<std::expected<void, RouteError>> joined;
    second.router->join(room_, mallory, [&](auto r) noexcept { joined = r; });
    ulw::test::pump_pending(*reactor_);
    EXPECT_FALSE(joined);
    EXPECT_EQ(db_.rooms.at(room_).generation, 1U);
    EXPECT_TRUE(first.router->healthy());
    EXPECT_EQ(send(first, alice, "alice", "still mine"), 1U);
}

TEST_P(RoomRouterTest, NoRoomIsClaimedBeforeTheNodeHasAdvertised) {
    Node& a = start("chat-a");
    Member alice;
    ASSERT_TRUE(join(a, alice));
    a.store->reachable = false;
    db_.make_stale(room_);
    Node& b = start("chat-b", kSecret, {.refuse_advertise = true});
    Member bob;
    std::optional<std::expected<void, RouteError>> joined;
    b.router->join(room_, bob, [&](auto r) noexcept { joined = r; });
    // A beat and a half: chat-b's first sweep would have run by now.
    ulw::test::pump_for(*reactor_, std::chrono::milliseconds(1'500));
    EXPECT_EQ(db_.rooms.at(room_).owner, *core::NodeId::parse("chat-a"));
    EXPECT_FALSE(joined);

    b.store->refuse_advertise = false;
    ASSERT_TRUE(pump([&] { return joined.has_value(); }));
    EXPECT_TRUE(*joined);
    EXPECT_EQ(db_.rooms.at(room_).owner, *core::NodeId::parse("chat-b"));
}

TEST_P(RoomRouterTest, AFloodOfIdleConnectionsCannotCrowdOutARealNode) {
    Node& a = start("chat-a");
    Member alice;
    ASSERT_TRUE(join(a, alice));
    // More idle connections than there are peer slots in all.
    std::vector<std::unique_ptr<RawPeer>> flood;
    for (int i = 0; i < 64; ++i) {
        flood.push_back(std::make_unique<RawPeer>(*reactor_, a.port));
        ulw::test::pump_pending(*reactor_);
    }
    // The oldest were dropped to make room for the newer, never the other way round.
    EXPECT_TRUE(flood.front()->hung_up());
    EXPECT_GT(a.router->counters().handshakes_evicted, 0U);

    Node& b = start("chat-b");
    Member bob;
    EXPECT_TRUE(join(b, bob));
    EXPECT_EQ(send(b, bob, "bob", "through the flood"), 1U);
}

INSTANTIATE_TEST_SUITE_P(Reactors, RoomRouterTest,
                         ::testing::Values(ReactorKind::IoUring, ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
