#include "core/ports/message_store.hpp"
#include "core/util/json.hpp"
#include "net/reactor_factory.hpp"

#include "presence.hpp"
#include "presence_room.hpp"
#include "support/fake_clock.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using core::Millis;

core::UserId user(std::string_view name) {
    return *core::UserId::parse(name);
}

// The whole cluster's room plane in one: joins are answered with the room's head, and sends
// are sequenced in the order they came and delivered to every member of the room, whichever
// node it is on, before the sender hears its seq. Nothing happens until settle().
class Plane final : public chat::IRooms {
public:
    void join(const core::RoomId& room, rt::IMember& member, rt::JoinCallback done) override {
        joins_.push_back({.room = room, .member = &member, .done = std::move(done)});
    }
    void leave(const core::RoomId& room, rt::IMember& member) noexcept override {
        std::erase(members_[room], &member);
        std::erase_if(joins_, [&](const Joining& j) { return j.member == &member; });
        std::erase_if(sends_, [&](const Sending& s) { return s.from == &member; });
    }
    void send(const core::RoomId& room, rt::IMember& from, const core::UserId& sender,
              const rt::MessageKey& key, std::vector<std::byte> body,
              rt::SendCallback done) override {
        sends_.push_back({.room = room,
                          .from = &from,
                          .sender = sender,
                          .key = key,
                          .body = std::move(body),
                          .done = std::move(done)});
    }
    // Presence never asks a room's owner anything.
    void ask_owner(const core::RoomId& /*room*/, rt::IMember& /*from*/,
                   std::span<const std::byte> /*request*/, rt::OwnerAnswer done) override {
        done(std::unexpected(rt::RouteError::Unavailable));
    }

    void settle() {
        while (!joins_.empty() || !sends_.empty()) {
            std::vector<Joining> joins = std::move(joins_);
            joins_.clear();
            for (Joining& j : joins) {
                members_[j.room].push_back(j.member);
                j.done(heads_[j.room]);
            }
            std::vector<Sending> sends = std::move(sends_);
            sends_.clear();
            for (Sending& s : sends) {
                if (failing > 0) {
                    --failing;
                    s.done(std::unexpected(rt::RouteError::Unavailable));
                    continue;
                }
                const std::uint64_t seq = ++heads_[s.room];
                ++sequenced[s.room];
                std::vector<rt::IMember*> to = members_[s.room];
                if (const auto lost = losing_.find(s.room); lost != losing_.end()) {
                    std::erase(to, lost->second);
                    losing_.erase(lost);
                }
                for (Dropping& d : dropping_) {
                    if (d.room == s.room && d.from == s.from && d.count > 0) {
                        std::erase(to, d.to);
                        --d.count;
                    }
                }
                for (rt::IMember* m : to) {
                    m->deliver({.room = s.room,
                                .seq = seq,
                                .sender = s.sender,
                                .key = s.key,
                                .body = s.body});
                }
                s.done(seq);
            }
        }
    }

    [[nodiscard]] std::size_t members(const core::RoomId& room) { return members_[room].size(); }

    // The room's next event reaches every member but the one that joined `nth` (from 0), as
    // when an owner dies between its deliveries.
    void lose_next(const core::RoomId& room, std::size_t nth) {
        losing_.insert_or_assign(room, members_[room].at(nth));
    }

    // The next `count` events the member that joined `from`-th sends reach every member but
    // the `to`-th, as on a path that keeps failing between the two.
    void drop_between(const core::RoomId& room, std::size_t from, std::size_t to, int count) {
        dropping_.push_back({.room = room,
                             .from = members_[room].at(from),
                             .to = members_[room].at(to),
                             .count = count});
    }

    // Another node takes the room over. Sends still waiting for their seq are lost with the old
    // owner (their senders hear unavailable), and the new owner answers joins from where the
    // store's count stands, as rt::RoomRegistry::taken_at has it.
    void new_owner(const core::RoomId& room) {
        std::vector<Sending> lost;
        std::erase_if(sends_, [&](Sending& s) {
            if (s.room != room) {
                return false;
            }
            lost.push_back(std::move(s));
            return true;
        });
        for (Sending& s : lost) {
            s.done(std::unexpected(rt::RouteError::Unavailable));
        }
    }

    // Sends to answer `unavailable` before sequencing any.
    int failing = 0;
    std::map<core::RoomId, std::uint64_t> sequenced;

private:
    struct Joining {
        core::RoomId room;
        rt::IMember* member;
        rt::JoinCallback done;
    };
    struct Sending {
        core::RoomId room;
        rt::IMember* from;
        core::UserId sender;
        rt::MessageKey key;
        std::vector<std::byte> body;
        rt::SendCallback done;
    };

    std::vector<Joining> joins_;
    std::vector<Sending> sends_;
    std::map<core::RoomId, std::vector<rt::IMember*>> members_;
    std::map<core::RoomId, std::uint64_t> heads_;
    std::map<core::RoomId, rt::IMember*> losing_;
    struct Dropping {
        core::RoomId room;
        rt::IMember* from;
        rt::IMember* to;
        int count;
    };
    std::vector<Dropping> dropping_;
};

// Who shares a direct or group chat with whom, as the message store would answer it: everyone
// with everyone unless `everyone` is cleared, then the pairs in `pairs`. Each answer is read when
// asked, as the store reads it, and waits for settle(), as the store's come on a later turn of
// the loop; while `holding`, they wait for release().
class Shares final : public chat::IPresenceAccess {
public:
    void shared_with(const core::UserId& user, std::vector<core::UserId> others,
                     core::ports::MessageCallback<std::vector<core::UserId>> done) override {
        ++asked;
        core::ports::MessageResult<std::vector<core::UserId>> answer =
            std::unexpected(core::ports::MessageStoreError::Unavailable);
        if (!failing) {
            std::vector<core::UserId> out;
            for (const core::UserId& other : others) {
                if (other != user && shared(user.view(), other.view())) {
                    out.push_back(other);
                }
            }
            answer = std::move(out);
        }
        pending_.push_back({.answer = std::move(answer), .done = std::move(done)});
    }

    [[nodiscard]] bool shared(std::string_view a, std::string_view b) const {
        return everyone ||
               pairs.contains({std::string(std::min(a, b)), std::string(std::max(a, b))});
    }
    void share(std::string_view a, std::string_view b) {
        pairs.emplace(std::string(std::min(a, b)), std::string(std::max(a, b)));
    }
    void unshare(std::string_view a, std::string_view b) {
        pairs.erase({std::string(std::min(a, b)), std::string(std::max(a, b))});
    }

    void settle() {
        if (!holding) {
            release();
        }
    }

    // Answers everything asked so far.
    void release() {
        std::vector<Pending> now = std::move(pending_);
        pending_.clear();
        for (Pending& p : now) {
            p.done(std::move(p.answer));
        }
    }

    [[nodiscard]] std::size_t waiting() const noexcept { return pending_.size(); }

    bool everyone = true;
    bool holding = false;
    bool failing = false;
    std::size_t asked = 0;
    std::set<std::pair<std::string, std::string>> pairs;

private:
    struct Pending {
        core::ports::MessageResult<std::vector<core::UserId>> answer;
        core::ports::MessageCallback<std::vector<core::UserId>> done;
    };
    std::vector<Pending> pending_;
};

struct Event {
    std::string type;
    std::string user;
    std::string status;
    std::string reason;
    friend bool operator==(const Event&, const Event&) = default;
};

std::ostream& operator<<(std::ostream& os, const Event& e) {
    return os << e.type << ' ' << e.user << ' ' << e.status << e.reason;
}

class Watcher final : public chat::IClient {
public:
    bool push(std::string_view text) noexcept override {
        const auto json = core::json::parse(text);
        const auto field = [&](std::string_view key) {
            const core::json::Value* v = json ? json->find(key) : nullptr;
            return std::string(v == nullptr ? "" : v->as_string().value_or(""));
        };
        got.push_back({.type = field("type"),
                       .user = field("user"),
                       .status = field("status"),
                       .reason = field("reason")});
        return true;
    }
    [[nodiscard]] std::size_t unsent_bytes() const noexcept override { return 0; }
    void allocation_failed() noexcept override {}

    std::vector<Event> take() { return std::exchange(got, {}); }

    std::vector<Event> got;
};

Event presence(std::string_view who, std::string_view status) {
    return {
        .type = "presence", .user = std::string(who), .status = std::string(status), .reason = {}};
}

Event watching(std::string_view who, std::string_view status) {
    return {
        .type = "watching", .user = std::string(who), .status = std::string(status), .reason = {}};
}

class PresenceTest : public ::testing::Test {
protected:
    static constexpr Millis kGrace{10'000};

    void SetUp() override {
        auto r = net::make_reactor(net::ReactorKind::Epoll, clock_, 64);
        ASSERT_TRUE(r);
        reactor_ = std::move(*r);
        for (const char* name : {"chat-1", "chat-2", "chat-3"}) {
            add_node(name);
        }
    }

    void add_node(std::string_view name, chat::PresenceLimits limits = {}) {
        nodes_.push_back(std::make_unique<chat::Presence>(plane_, shares_, *reactor_, clock_,
                                                          *core::NodeId::parse(name), limits));
    }

    chat::Presence& node(std::size_t i) { return *nodes_.at(i); }

    // Everything due now, and everything that follows from it, happens.
    void run() {
        for (int i = 0; i < 16; ++i) {
            reactor_->run_once(Millis{0});
            plane_.settle();
            shares_.settle();
        }
    }

    void advance(Millis d) {
        // A second at a time, as the service scans: every deadline in `d` comes due in order.
        while (d > Millis{0}) {
            const Millis step = std::min(d, Millis{1'000});
            clock_.advance(step);
            d -= step;
            run();
        }
    }

    chat::PresenceClientId connect(std::size_t at, chat::IClient& client, std::string_view who) {
        const auto id = node(at).attach(client, user(who));
        run();
        return id;
    }

    [[nodiscard]] std::uint64_t sent() const {
        std::uint64_t n = 0;
        for (const auto& p : nodes_) {
            n += p->counters().sent;
        }
        return n;
    }

    ulw::test::FakeClock clock_;
    std::unique_ptr<net::IReactor> reactor_;
    Plane plane_;
    Shares shares_;
    std::vector<std::unique_ptr<chat::Presence>> nodes_;
};

TEST(PresenceRoom, EveryNodeDerivesTheSameVersion8RoomForAUserAndAnotherForAnother) {
    const core::RoomId alice = chat::presence_room(user("alice"));
    EXPECT_EQ(alice, chat::presence_room(user("alice")));
    EXPECT_NE(alice, chat::presence_room(user("alicf")));
    EXPECT_TRUE(chat::is_presence_room(alice));
    EXPECT_EQ(alice.to_string()[14], '8');
    EXPECT_FALSE(
        chat::is_presence_room(*core::RoomId::parse("01a0eb86-6cca-7dce-84cc-3bb47615f9fd")));
}

// Pinned: every node of every version must derive the same room for a user, and its first byte
// is the presence tag, never a stream chat's (ADR-0056), so the two kinds of derived room never
// share an id.
TEST(PresenceRoom, AUsersRoomIsPinnedAndTaggedAsAPresenceRoom) {
    using core::ports::NamedRoom;
    const core::RoomId alice = chat::presence_room(user("alice"));
    // SHA-256 of "ulw presence room\nalice": byte 0 set to 0x02, then the version and variant.
    EXPECT_EQ(alice.to_string(), "0245c53c-9c67-87a9-b59f-58fd4886aa2e");
    EXPECT_EQ(static_cast<std::uint8_t>(NamedRoom::Presence), 0x02U);
    EXPECT_EQ(static_cast<std::uint8_t>(NamedRoom::StreamChat), 0x01U);
    EXPECT_EQ(std::to_integer<std::uint8_t>(alice.uuid().bytes()[0]),
              static_cast<std::uint8_t>(NamedRoom::Presence));
    EXPECT_TRUE(core::ports::is_named_room(alice, NamedRoom::Presence));
    EXPECT_FALSE(core::ports::is_named_room(alice, NamedRoom::StreamChat));
    EXPECT_FALSE(core::ports::is_stream_chat(alice));
    for (const char* name : {"bob", "carol", "auth0|dave"}) {
        const auto u = core::UserId::parse(name);
        ASSERT_TRUE(u) << name;
        EXPECT_TRUE(core::ports::is_named_room(chat::presence_room(*u), NamedRoom::Presence))
            << name;
    }
    // A stream chat's derived id, version 8 and tagged 0x01, is not a presence room.
    const core::RoomId stream = *core::RoomId::parse("01a0eb86-6cca-8dce-84cc-3bb47615f9fd");
    EXPECT_TRUE(core::ports::is_stream_chat(stream));
    EXPECT_FALSE(chat::is_presence_room(stream));
}

TEST_F(PresenceTest, AUserNobodyWatchesCostsNoEventAndNoRoomOnceTheGraceIsOver) {
    Watcher alice;
    const auto a = connect(0, alice, "alice");
    EXPECT_EQ(plane_.members(chat::presence_room(user("alice"))), 1U)
        << "joined, to hear anyone who starts watching";
    node(0).detach(a);
    run();
    advance(kGrace + Millis{1'000});
    EXPECT_EQ(sent(), 0U);
    EXPECT_TRUE(plane_.sequenced.empty());
    EXPECT_EQ(node(0).rooms(), 0U);
    EXPECT_EQ(plane_.members(chat::presence_room(user("alice"))), 0U);
    EXPECT_TRUE(alice.got.empty());
}

TEST_F(PresenceTest, WatchersOnEveryNodeHearOnlineAndThenExactlyOneOfflineAfterTheGrace) {
    std::vector<Watcher> bobs(3);
    for (std::size_t i = 0; i < 3; ++i) {
        const auto b = connect(i, bobs[i], "bob");
        node(i).watch(b, user("alice"));
        run();
        EXPECT_EQ(bobs[i].take(), std::vector{watching("alice", "offline")}) << i;
    }
    Watcher alice;
    const auto a = connect(1, alice, "alice");
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(bobs[i].take(), std::vector{presence("alice", "online")}) << i;
    }

    node(1).detach(a);
    run();
    advance(kGrace - Millis{1'000});
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_TRUE(bobs[i].got.empty()) << i << " heard before the grace ran out";
    }
    advance(Millis{2'000});
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(bobs[i].take(), std::vector{presence("alice", "offline")}) << i;
    }
    advance(Millis{300'000});
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_TRUE(bobs[i].got.empty()) << i;
    }
    EXPECT_EQ(plane_.sequenced[chat::presence_room(user("alice"))], 3U + 1U + 2U + 1U)
        << "three hellos, the probe, two acks and the offline";
}

TEST_F(PresenceTest, AReconnectWithinTheGraceProducesNoEventAndNoMessage) {
    Watcher bob;
    node(2).watch(connect(2, bob, "bob"), user("alice"));
    Watcher alice;
    auto a = connect(0, alice, "alice");
    ASSERT_EQ(bob.take(), (std::vector{watching("alice", "offline"), presence("alice", "online")}));
    const std::uint64_t before = sent();

    // Within the minute before her announcement is renewed, which is a message of its own.
    for (int flap = 0; flap < 2; ++flap) {
        node(0).detach(a);
        run();
        advance(kGrace - Millis{2'000});
        a = connect(0, alice, "alice");
    }
    node(0).detach(a);
    run();
    advance(kGrace - Millis{2'000});
    a = connect(0, alice, "alice");
    advance(kGrace + Millis{2'000});
    EXPECT_TRUE(bob.got.empty());
    EXPECT_EQ(sent(), before);
}

TEST_F(PresenceTest, AReconnectThroughAnotherNodeWithinTheGraceProducesNoEvent) {
    Watcher bob;
    node(2).watch(connect(2, bob, "bob"), user("alice"));
    Watcher alice;
    const auto first = connect(0, alice, "alice");
    ASSERT_EQ(bob.take(), (std::vector{watching("alice", "offline"), presence("alice", "online")}));

    node(0).detach(first);
    run();
    advance(Millis{1'000});
    const auto second = connect(1, alice, "alice");
    // chat-1's grace runs out and it says so; chat-2 still has her.
    advance(kGrace * 2);
    EXPECT_TRUE(bob.got.empty());
    EXPECT_GE(node(0).counters().sent, 1U) << "chat-1 announced her offline for itself";

    node(1).detach(second);
    run();
    advance(kGrace + Millis{1'000});
    EXPECT_EQ(bob.take(), std::vector{presence("alice", "offline")});
}

TEST_F(PresenceTest, AWatcherArrivingAfterTheUserConnectedHearsThatTheyAreOnline) {
    Watcher alice;
    connect(0, alice, "alice");
    EXPECT_EQ(sent(), 0U);
    Watcher bob;
    node(1).watch(connect(1, bob, "bob"), user("alice"));
    run();
    EXPECT_EQ(bob.take(), (std::vector{watching("alice", "offline"), presence("alice", "online")}));
    // A second watcher on the same node is answered from what the node knows.
    Watcher carol;
    node(1).watch(connect(1, carol, "carol"), user("alice"));
    run();
    EXPECT_EQ(carol.take(), std::vector{watching("alice", "online")});
}

TEST_F(PresenceTest, AWatcherOnTheUsersOwnNodeSeesTheSameChangesAsOneElsewhere) {
    Watcher here;
    Watcher there;
    node(0).watch(connect(0, here, "bob"), user("alice"));
    node(1).watch(connect(1, there, "carol"), user("alice"));
    run();
    Watcher alice;
    const auto a = connect(0, alice, "alice");
    node(0).detach(a);
    run();
    advance(kGrace + Millis{1'000});
    const std::vector expected{watching("alice", "offline"), presence("alice", "online"),
                               presence("alice", "offline")};
    EXPECT_EQ(here.take(), expected);
    EXPECT_EQ(there.take(), expected);
}

TEST_F(PresenceTest, ANodeThatStopsRenewingIsForgottenAfterTheExpiryAndOnlyThen) {
    Watcher bob;
    node(1).watch(connect(1, bob, "bob"), user("alice"));
    Watcher alice;
    connect(0, alice, "alice");
    ASSERT_EQ(bob.take(), (std::vector{watching("alice", "offline"), presence("alice", "online")}));

    // Renewed every minute, she stays online however long she stays.
    advance(Millis{600'000});
    EXPECT_TRUE(bob.got.empty());

    // chat-1 dies: it leaves nothing behind, and says nothing. Its last renewal was at most a
    // minute ago, so its announcement stands for 90 s more at least, and 150 s at most.
    nodes_[0].reset();
    run();
    advance(Millis{89'000});
    EXPECT_TRUE(bob.got.empty()) << "forgotten before its announcement expired";
    advance(Millis{62'000});
    EXPECT_EQ(bob.take(), std::vector{presence("alice", "offline")});
    EXPECT_EQ(node(1).counters().expired, 1U);
}

TEST_F(PresenceTest, AFailedAnnouncementIsSentAgainAndStillReportedOnce) {
    Watcher bob;
    node(1).watch(connect(1, bob, "bob"), user("alice"));
    run();
    ASSERT_EQ(bob.take(), std::vector{watching("alice", "offline")});
    plane_.failing = 2;
    Watcher alice;
    const auto a = connect(0, alice, "alice");
    EXPECT_TRUE(bob.got.empty());
    advance(Millis{2'000});
    EXPECT_EQ(bob.take(), std::vector{presence("alice", "online")});

    node(0).detach(a);
    run();
    plane_.failing = 1;
    advance(kGrace + Millis{3'000});
    EXPECT_EQ(bob.take(), std::vector{presence("alice", "offline")});
}

TEST_F(PresenceTest, AnUnwatchedUserStopsBeingReportedAndItsRoomIsLeft) {
    Watcher bob;
    const auto b = connect(1, bob, "bob");
    node(1).watch(b, user("alice"));
    run();
    node(1).unwatch(b, user("alice"));
    run();
    EXPECT_EQ(node(1).rooms(), 1U) << "only bob's own";
    Watcher alice;
    connect(0, alice, "alice");
    EXPECT_EQ(bob.take(), std::vector{watching("alice", "offline")});
}

TEST_F(PresenceTest, WatchesPastTheLimitsAreRefusedNamingTheUser) {
    nodes_.clear();
    chat::PresenceLimits limits;
    limits.max_watches_per_client = 2;
    limits.max_rooms = 4;
    add_node("chat-1", limits);
    Watcher bob;
    const auto b = connect(0, bob, "bob");
    for (const char* who : {"u1", "u2", "u3"}) {
        node(0).watch(b, user(who));
    }
    EXPECT_EQ(bob.take().back(),
              (Event{.type = "error", .user = "u3", .status = {}, .reason = "too_many_watches"}));
    Watcher carol;
    const auto c = connect(0, carol, "carol");
    node(0).watch(c, user("u1"));
    run();
    EXPECT_EQ(carol.take(), std::vector{watching("u1", "offline")})
        << "a room already here is free";
    node(0).watch(c, user("u9"));
    EXPECT_TRUE(carol.got.empty()) << "the node's room cap is applied after the store's answer";
    run();
    EXPECT_EQ(carol.take(),
              std::vector{(Event{.type = "error", .user = "u9", .status = {}, .reason = "busy"})})
        << "bob, carol, u1 and u2 fill the node's four rooms";
}

TEST_F(PresenceTest, WatchingTheSameUserTwiceAnswersTwiceAndJoinsOnce) {
    Watcher bob;
    const auto b = connect(1, bob, "bob");
    node(1).watch(b, user("alice"));
    node(1).watch(b, user("alice"));
    run();
    EXPECT_EQ(bob.take(),
              (std::vector{watching("alice", "offline"), watching("alice", "offline")}));
    EXPECT_EQ(plane_.members(chat::presence_room(user("alice"))), 1U);
    EXPECT_EQ(plane_.sequenced[chat::presence_room(user("alice"))], 1U) << "one hello";
}

TEST_F(PresenceTest, AStandingHelloIsAnsweredByAUserWhoConnectsAfterTheRoomChangedOwners) {
    Watcher bob;
    node(1).watch(connect(1, bob, "bob"), user("alice"));
    run();
    ASSERT_EQ(bob.take(), std::vector{watching("alice", "offline")});
    plane_.new_owner(chat::presence_room(user("alice")));
    Watcher alice;
    connect(0, alice, "alice");
    EXPECT_EQ(bob.take(), std::vector{presence("alice", "online")})
        << "the new owner's join answer showed the hello, and alice's node probed";
}

TEST_F(PresenceTest, AWatcherThatMissedAnAnnouncementSaysHelloAgainAtTheGap) {
    const core::RoomId room = chat::presence_room(user("alice"));
    Watcher bob;
    Watcher carol;
    node(1).watch(connect(1, bob, "bob"), user("alice"));
    run();
    node(2).watch(connect(2, carol, "carol"), user("alice"));
    run();
    bob.take();
    carol.take();
    // chat-2 joined the room first. alice's probe reaches everyone but it; chat-3's ack to
    // the probe is the next thing chat-2 hears, a seq past the one it last saw.
    plane_.lose_next(room, 0);
    Watcher alice;
    connect(0, alice, "alice");
    EXPECT_EQ(carol.take(), std::vector{presence("alice", "online")});
    EXPECT_EQ(bob.take(), std::vector{presence("alice", "online")})
        << "not a minute later, at the renewal";
    EXPECT_EQ(node(1).counters().gaps, 1U);
}

TEST_F(PresenceTest, WatchingAndUnwatchingInALoopIsHeldToTheWatchBucket) {
    Watcher bob;
    connect(1, bob, "bob");
    Watcher mallory;
    const auto m = connect(1, mallory, "mallory");
    const std::uint64_t before = sent();
    for (int i = 0; i < 1'000; ++i) {
        node(1).watch(m, user("bob"));
        run();
        node(1).unwatch(m, user("bob"));
        run();
    }
    // The burst of 128 starts, each a hello, bob's node answering it, and an unwatch; the
    // other 872 are refused.
    EXPECT_EQ(sent() - before, 3U * 128U);
    const auto refused = std::ranges::count(mallory.got, "busy", &Event::reason);
    EXPECT_EQ(refused, 1'000 - 128);
}

TEST_F(PresenceTest, AUserCannotWatchThemselves) {
    Watcher alice;
    node(0).watch(connect(0, alice, "alice"), user("alice"));
    run();
    EXPECT_EQ(alice.take(),
              std::vector{(Event{
                  .type = "error", .user = "alice", .status = {}, .reason = "watching_self"})});
    EXPECT_EQ(sent(), 0U);
}

TEST_F(PresenceTest, RenewalsStopOnceTheOnlyNodeWatchingHasDied) {
    Watcher bob;
    node(1).watch(connect(1, bob, "bob"), user("alice"));
    Watcher alice;
    connect(0, alice, "alice");
    advance(Millis{120'000});
    const std::uint64_t renewing = node(0).counters().sent;
    EXPECT_GE(renewing, 3U) << "the probe and two renewals";
    // chat-2 dies; nothing more acks chat-1's renewals, and after the expiry it stops.
    nodes_[1].reset();
    run();
    advance(Millis{200'000});
    const std::uint64_t stopped = node(0).counters().sent;
    EXPECT_LE(stopped - renewing, 3U);
    advance(Millis{600'000});
    EXPECT_EQ(node(0).counters().sent, stopped);
}

TEST_F(PresenceTest, AWatcherWhoseAcksAreLostIsProbedAgainAndNeverSeesAFalseOffline) {
    const core::RoomId room = chat::presence_room(user("alice"));
    Watcher bob;
    node(1).watch(connect(1, bob, "bob"), user("alice"));
    run();
    Watcher alice;
    connect(0, alice, "alice");
    ASSERT_EQ(bob.take(), (std::vector{watching("alice", "offline"), presence("alice", "online")}));
    // chat-2 joined first, chat-1 second. chat-2's next two acks never reach chat-1: had it
    // taken the jumps they leave for renewals it need not ack, it would forget chat-2 at the
    // expiry and stop renewing, and chat-2 would then drop alice while she is still here.
    plane_.drop_between(room, 0, 1, 2);
    advance(Millis{900'000});
    EXPECT_TRUE(bob.got.empty());
    EXPECT_GE(node(0).counters().gaps, 1U);
}

// Who may see whose presence (ADR-0096): those who share a direct or group chat, now.

Event refused(std::string_view who, std::string_view reason) {
    return {.type = "error", .user = std::string(who), .status = {}, .reason = std::string(reason)};
}

// Any group chat's id: the removals below name the room they came from, which presence does not
// look at beyond its kind.
const core::RoomId kGroup = *core::RoomId::parse("04ff0817-879c-8a63-b5a8-9ebacad5d927");

TEST_F(PresenceTest, AWatchOfSomeoneWhoSharesNoChatIsRefusedAndCostsNoEvent) {
    shares_.everyone = false;
    Watcher alice;
    connect(0, alice, "alice");
    Watcher bob;
    const auto b = connect(1, bob, "bob");
    node(1).watch(b, user("alice"));
    EXPECT_TRUE(bob.got.empty()) << "nothing before the store answers";
    run();
    EXPECT_EQ(bob.take(), std::vector{refused("alice", "not_shared")});
    EXPECT_EQ(sent(), 0U) << "no hello: the watch was never let in";
    EXPECT_EQ(plane_.members(chat::presence_room(user("alice"))), 1U) << "alice's own node only";
    EXPECT_EQ(node(1).counters().not_shared, 1U);
    // Refused again from memory, without asking, until either is listed somewhere.
    const std::size_t asked = shares_.asked;
    shares_.share("alice", "bob");
    node(1).watch(b, user("alice"));
    EXPECT_EQ(bob.take(), std::vector{refused("alice", "not_shared")});
    EXPECT_EQ(shares_.asked, asked);
    // Once they share a chat, the same watch is let in.
    node(1).on_member_added(kGroup, user("bob"));
    node(1).watch(b, user("alice"));
    run();
    EXPECT_EQ(bob.take(), (std::vector{watching("alice", "offline"), presence("alice", "online")}));
}

TEST_F(PresenceTest, ARemovalHoldsTheWatchBackAndDropsItOnceNoChatIsShared) {
    shares_.everyone = false;
    shares_.share("alice", "bob");
    Watcher bob;
    const auto b = connect(1, bob, "bob");
    node(1).watch(b, user("alice"));
    run();
    Watcher alice;
    const auto a = connect(0, alice, "alice");
    ASSERT_EQ(bob.take(), (std::vector{watching("alice", "offline"), presence("alice", "online")}));
    // Bob is taken off their only shared chat; every node hears it, and presence holds his
    // watch back at once, before the store has answered again.
    shares_.holding = true;
    shares_.unshare("alice", "bob");
    node(1).on_member_removed(kGroup, user("bob"));
    node(0).detach(a);
    run();
    advance(kGrace + Millis{1'000});
    EXPECT_TRUE(bob.got.empty()) << "nothing while held";
    shares_.release();
    run();
    EXPECT_EQ(bob.take(), std::vector{refused("alice", "not_shared")});
    EXPECT_EQ(node(1).counters().revoked, 1U);
    connect(0, alice, "alice");
    advance(Millis{2'000});
    EXPECT_TRUE(bob.got.empty()) << "the watch is gone";
}

TEST_F(PresenceTest, TheRemovedUsersWatchesAndWatchersAreBothCheckedAgain) {
    shares_.everyone = false;
    shares_.share("alice", "bob");
    shares_.share("bob", "carol");
    Watcher alice;
    Watcher bob;
    Watcher carol;
    const auto a = connect(0, alice, "alice");
    const auto b = connect(0, bob, "bob");
    const auto c = connect(0, carol, "carol");
    node(0).watch(a, user("bob"));
    node(0).watch(b, user("carol"));
    node(0).watch(c, user("bob"));
    run();
    for (Watcher* w : {&alice, &bob, &carol}) {
        w->take();
    }
    const std::size_t before = shares_.asked;
    // Bob leaves the group he shared with carol: his own watch of carol and carol's of him are in
    // doubt; alice's of bob as well, since nothing here knows which room she shared with him.
    shares_.unshare("bob", "carol");
    node(0).on_member_removed(kGroup, user("bob"));
    run();
    EXPECT_EQ(shares_.asked - before, 3U);
    EXPECT_TRUE(alice.got.empty());
    EXPECT_EQ(bob.take(), std::vector{refused("carol", "not_shared")});
    EXPECT_EQ(carol.take(), std::vector{refused("bob", "not_shared")});
    // A stream's live chat lists nobody: a removal from one changes nothing here.
    node(0).on_member_removed(*core::RoomId::parse("011b9ed0-d6b6-88e6-ac34-32d7070ba83b"),
                              user("alice"));
    run();
    EXPECT_EQ(shares_.asked - before, 3U);
}

TEST_F(PresenceTest, AHeldWatchStillSharedComesBackWithWhatChangedMeanwhile) {
    Watcher bob;
    const auto b = connect(1, bob, "bob");
    node(1).watch(b, user("alice"));
    run();
    ASSERT_EQ(bob.take(), std::vector{watching("alice", "offline")});
    shares_.holding = true;
    node(1).on_member_removed(kGroup, user("alice"));
    Watcher alice;
    connect(0, alice, "alice");
    EXPECT_TRUE(bob.got.empty());
    shares_.release();
    run();
    EXPECT_EQ(bob.take(), std::vector{presence("alice", "online")});
    // Watched again while held: answered once let back in, with the state it then has.
    node(1).on_members_resync();
    node(1).watch(b, user("alice"));
    node(1).watch(b, user("alice"));
    run();
    EXPECT_TRUE(bob.got.empty());
    shares_.release();
    run();
    EXPECT_EQ(bob.take(), (std::vector{watching("alice", "online"), watching("alice", "online")}));
    EXPECT_EQ(node(1).counters().revoked, 0U);
}

TEST_F(PresenceTest, ARemovalWhileAWatchIsAskedAsksAgainBeforeLettingItIn) {
    shares_.everyone = false;
    shares_.share("alice", "bob");
    shares_.holding = true;
    Watcher bob;
    const auto b = connect(1, bob, "bob");
    node(1).watch(b, user("alice"));
    // Read as shared; then bob is removed before the answer arrives.
    shares_.unshare("alice", "bob");
    node(1).on_member_removed(kGroup, user("bob"));
    shares_.release();
    run();
    EXPECT_TRUE(bob.got.empty()) << "the answer may predate the removal: asked again";
    EXPECT_EQ(shares_.waiting(), 1U);
    shares_.release();
    run();
    EXPECT_EQ(bob.take(), std::vector{refused("alice", "not_shared")});
    EXPECT_EQ(sent(), 0U);
}

TEST_F(PresenceTest, AResyncChecksEveryWatchAgain) {
    shares_.everyone = false;
    shares_.share("alice", "bob");
    shares_.share("alice", "carol");
    Watcher alice;
    const auto a = connect(2, alice, "alice");
    node(2).watch(a, user("bob"));
    node(2).watch(a, user("carol"));
    run();
    ASSERT_EQ(alice.take().size(), 2U);
    shares_.unshare("alice", "carol");
    node(2).on_members_resync();
    run();
    EXPECT_EQ(alice.take(), std::vector{refused("carol", "not_shared")});
    Watcher bob;
    connect(0, bob, "bob");
    EXPECT_EQ(alice.take(), std::vector{presence("bob", "online")});
}

// Fail closed, but keep the watch: one the store cannot check stays held, telling nothing, and
// is asked again later, later each time, until the store answers.
TEST_F(PresenceTest, AStoreThatCannotSayRefusesANewWatchAndHoldsAWatchInDoubtUntilItCan) {
    Watcher bob;
    const auto b = connect(1, bob, "bob");
    node(1).watch(b, user("carol"));
    run();
    ASSERT_EQ(bob.take(), std::vector{watching("carol", "offline")});
    shares_.failing = true;
    node(1).watch(b, user("alice"));
    run();
    EXPECT_EQ(bob.take(), std::vector{refused("alice", "unavailable")});
    std::size_t asked = shares_.asked;
    node(1).on_member_removed(kGroup, user("carol"));
    run();
    EXPECT_EQ(shares_.asked, asked + 1);
    Watcher carol;
    const auto c = connect(0, carol, "carol");
    EXPECT_TRUE(bob.got.empty()) << "held: nothing told, nothing dropped";
    // Asked again after 1 s, then 2 s, then 4 s.
    for (const auto wait : {Millis{1'000}, Millis{2'000}, Millis{4'000}}) {
        asked = shares_.asked;
        advance(wait - Millis{1'000});
        EXPECT_EQ(shares_.asked, asked) << wait.count();
        advance(Millis{1'000});
        EXPECT_EQ(shares_.asked, asked + 1) << wait.count();
    }
    EXPECT_TRUE(bob.got.empty());
    EXPECT_EQ(node(1).counters().revoked, 0U);
    shares_.failing = false;
    advance(Millis{8'000});
    EXPECT_EQ(bob.take(), std::vector{presence("carol", "online")})
        << "let back, with what changed while held";
    node(0).detach(c);
    advance(kGrace + Millis{1'000});
    EXPECT_EQ(bob.take(), std::vector{presence("carol", "offline")});
}

// A resync holds every watch at once and asks about them a wave of clients at a time.
TEST_F(PresenceTest, AResyncAsksAboutItsClientsInWaves) {
    std::vector<Watcher> watchers(70);
    for (std::size_t i = 0; i < watchers.size(); ++i) {
        node(1).watch(connect(1, watchers[i], std::format("w{}", i)), user("alice"));
    }
    run();
    const std::size_t asked = shares_.asked;
    node(1).on_members_resync();
    for (int i = 0; i < 16; ++i) {
        reactor_->run_once(Millis{0});
    }
    EXPECT_EQ(shares_.asked - asked, 32U) << "the first wave";
    advance(Millis{1'000});
    EXPECT_EQ(shares_.asked - asked, 70U);
    for (Watcher& w : watchers) {
        EXPECT_EQ(w.take(), (std::vector{watching("alice", "offline")}));
    }
}

// Every watch the store is asked about costs the allowance, whether or not anyone here watches
// the user already, so a refusal says nothing of who is watched (and the room cap comes after
// the store's answer).
TEST_F(PresenceTest, TheAllowanceIsChargedAlikeWhetherTheUserIsWatchedHereOrNot) {
    nodes_.clear();
    chat::PresenceLimits limits;
    limits.watch_burst = 2;
    limits.watches_per_second = 1;
    add_node("chat-1", limits);
    Watcher carol;
    node(0).watch(connect(0, carol, "carol"), user("alice"));
    run();
    ASSERT_EQ(carol.take(), std::vector{watching("alice", "offline")});
    Watcher bob;
    const auto b = connect(0, bob, "bob");
    node(0).watch(b, user("alice"));
    node(0).watch(b, user("dave"));
    node(0).watch(b, user("erin"));
    run();
    EXPECT_EQ(bob.take(), (std::vector{refused("erin", "busy"), watching("alice", "offline"),
                                       watching("dave", "offline")}));
}

TEST_F(PresenceTest, AWatchUnwatchedOrAClientGoneWhileAskedIsAnsweredNothing) {
    shares_.holding = true;
    Watcher bob;
    const auto b = connect(1, bob, "bob");
    node(1).watch(b, user("alice"));
    node(1).unwatch(b, user("alice"));
    Watcher carol;
    const auto c = connect(1, carol, "carol");
    node(1).watch(c, user("alice"));
    node(1).detach(c);
    shares_.release();
    run();
    EXPECT_TRUE(bob.got.empty());
    EXPECT_TRUE(carol.got.empty());
    EXPECT_EQ(sent(), 0U);
    EXPECT_EQ(node(1).rooms(), 2U) << "bob's own, and carol's in her grace: none for alice";
}

TEST_F(PresenceTest, PendingWatchesCountAgainstTheClientsLimit) {
    nodes_.clear();
    chat::PresenceLimits limits;
    limits.max_watches_per_client = 2;
    add_node("chat-1", limits);
    shares_.holding = true;
    Watcher bob;
    const auto b = connect(0, bob, "bob");
    node(0).watch(b, user("u1"));
    node(0).watch(b, user("u2"));
    node(0).watch(b, user("u3"));
    EXPECT_EQ(bob.take(), std::vector{refused("u3", "too_many_watches")});
    shares_.release();
    run();
    EXPECT_EQ(bob.take(), (std::vector{watching("u1", "offline"), watching("u2", "offline")}));
}

} // namespace
