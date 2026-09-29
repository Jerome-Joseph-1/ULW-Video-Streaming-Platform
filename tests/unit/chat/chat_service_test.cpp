#include "core/util/json.hpp"
#include "infra/auth/base64url.hpp"

#include "chat_service.hpp"
#include "live_chat.hpp"
#include "support/fake_clock.hpp"

#include <format>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using core::Millis;
using rt::RouteError;

constexpr std::string_view kRoom = "01a0eb86-6cca-7dce-84cc-3bb47615f9fd";
constexpr std::string_view kOtherRoom = "01a0eb86-6cca-7dce-84cc-3bb47615f9fe";

core::RoomId room_id(std::string_view text = kRoom) {
    return *core::RoomId::parse(text);
}

rt::MessageKey key(std::string_view text) {
    return *rt::MessageKey::parse(text);
}

std::vector<std::byte> bytes(std::string_view text) {
    const auto b = std::as_bytes(std::span{text});
    return {b.begin(), b.end()};
}

// The room plane, answered by hand: the test decides when a join or a send is answered, and
// what the owner sequences.
class FakeRooms final : public chat::IRooms {
public:
    struct Joining {
        core::RoomId room;
        rt::IMember* member;
        rt::JoinCallback done;
    };
    struct Sending {
        core::RoomId room;
        core::UserId sender;
        rt::MessageKey key;
        std::vector<std::byte> body;
        rt::SendCallback done;
    };

    void join(const core::RoomId& room, rt::IMember& member, rt::JoinCallback done) override {
        joins.push_back({.room = room, .member = &member, .done = std::move(done)});
    }
    void leave(const core::RoomId& room, rt::IMember& /*member*/) noexcept override {
        left.push_back(room);
    }
    void send(const core::RoomId& room, rt::IMember& /*from*/, const core::UserId& sender,
              const rt::MessageKey& key, std::vector<std::byte> body,
              rt::SendCallback done) override {
        sends.push_back({.room = room,
                         .sender = sender,
                         .key = key,
                         .body = std::move(body),
                         .done = std::move(done)});
    }

    // Answers the oldest join, and returns the member it made.
    rt::IMember& admit(std::expected<std::uint64_t, RouteError> result = 0) {
        Joining j = std::move(joins.front());
        joins.erase(joins.begin());
        j.done(result);
        return *j.member;
    }

    std::vector<Joining> joins;
    std::vector<Sending> sends;
    std::vector<core::RoomId> left;
};

class FakeClient final : public chat::IClient {
public:
    bool push(std::string_view text) noexcept override {
        if (closing) {
            return false;
        }
        got.emplace_back(text);
        unsent += growth;
        return true;
    }
    [[nodiscard]] std::size_t unsent_bytes() const noexcept override { return unsent; }
    void allocation_failed() noexcept override { ++failures; }

    // What arrived since the last call.
    std::vector<std::string> take() { return std::exchange(got, {}); }

    std::vector<std::string> got;
    std::size_t unsent = 0;
    // Added to unsent by every push, for a connection that is not reading.
    std::size_t growth = 0;
    bool closing = false;
    int failures = 0;
};

struct Seen {
    std::string type;
    std::uint64_t seq = 0;
    std::string id;
    std::string body;
    std::string reason;
    std::optional<std::uint64_t> retry_after_ms;
};

Seen seen(const std::string& text) {
    const auto json = core::json::parse(text);
    EXPECT_TRUE(json) << text;
    const auto string = [&](std::string_view field) {
        const core::json::Value* v = json->find(field);
        return std::string(v == nullptr ? "" : v->as_string().value_or(""));
    };
    Seen s{.type = string("type"),
           .seq = 0,
           .id = string("id"),
           .body = infra::auth::decode_base64url(string("body")).value_or("?"),
           .reason = string("reason"),
           .retry_after_ms = std::nullopt};
    if (const core::json::Value* v = json->find("seq")) {
        s.seq = v->as_u64().value_or(0);
    }
    if (const core::json::Value* v = json->find("retry_after_ms")) {
        s.retry_after_ms = v->as_u64();
    }
    return s;
}

std::vector<std::uint64_t> seqs(const std::vector<std::string>& texts) {
    std::vector<std::uint64_t> out;
    for (const std::string& t : texts) {
        const Seen s = seen(t);
        if (s.type == "message") {
            out.push_back(s.seq);
        }
    }
    return out;
}

class ChatServiceTest : public ::testing::Test {
protected:
    explicit ChatServiceTest(chat::ServiceLimits limits = {})
        : service_(std::make_unique<chat::ChatService>(rooms_, clock_, limits)) {}

    chat::ClientId attach(FakeClient& client, std::string_view user = "alice") {
        return service_->attach(client, *core::UserId::parse(user));
    }

    void join(chat::ClientId id, std::optional<std::uint64_t> after = std::nullopt,
              chat::Delivery delivery = chat::Delivery::Durable, std::string_view room = kRoom) {
        service_->join(id, {.room = room_id(room), .after = after, .delivery = delivery});
    }

    void send(chat::ClientId id, std::string_view id_text, std::string_view body = "hi",
              std::string_view room = kRoom) {
        service_->send(id, {.room = room_id(room), .id = key(id_text), .body = bytes(body)});
    }

    // What the room's owner would deliver: the next seq, from `sender`.
    static void deliver(rt::IMember& member, std::uint64_t seq, std::string_view body = "hi",
                        std::string_view sender = "bob", std::string_view room = kRoom) {
        const auto b = std::as_bytes(std::span{body});
        member.deliver({.room = room_id(room),
                        .seq = seq,
                        .sender = *core::UserId::parse(sender),
                        .key = key("m" + std::to_string(seq)),
                        .body = b});
    }

    FakeRooms rooms_;
    ulw::test::FakeClock clock_;
    std::unique_ptr<chat::ChatService> service_;
};

TEST_F(ChatServiceTest, ARoomIsJoinedOnceForAllItsClientsHereAndEachHearsEveryMessage) {
    FakeClient alice;
    FakeClient bob;
    const auto a = attach(alice);
    const auto b = attach(bob, "bob");
    join(a);
    join(b);
    ASSERT_EQ(rooms_.joins.size(), 1U);
    rt::IMember& member = rooms_.admit();
    EXPECT_EQ(seen(alice.take().at(0)).type, "joined");
    EXPECT_EQ(seen(bob.take().at(0)).type, "joined");

    // Any bytes at all: not text, not even valid UTF-8.
    const std::string_view body{"caf\xc3\xa9 \x00\xff", 8};
    deliver(member, 1, body, "bob");
    ASSERT_EQ(alice.got.size(), 1U);
    EXPECT_EQ(alice.got, bob.got);
    const Seen m = seen(alice.got[0]);
    EXPECT_EQ(m.type, "message");
    EXPECT_EQ(m.seq, 1U);
    EXPECT_EQ(m.id, "m1");
    EXPECT_EQ(m.body, body);
    EXPECT_EQ(service_->counters().delivered, 2U);
    // A connection on its way out takes nothing, and is not counted as delivered to.
    bob.closing = true;
    deliver(member, 2);
    EXPECT_EQ(service_->counters().delivered, 3U);
}

TEST_F(ChatServiceTest, ASendIsAnsweredWithItsIdAndTheSeqTheOwnerGaveIt) {
    FakeClient alice;
    const auto a = attach(alice);
    join(a);
    rooms_.admit();
    alice.take();
    send(a, "first", "opaque");
    ASSERT_EQ(rooms_.sends.size(), 1U);
    EXPECT_EQ(rooms_.sends[0].key, key("first"));
    EXPECT_EQ(rooms_.sends[0].sender, *core::UserId::parse("alice"));
    EXPECT_EQ(rooms_.sends[0].body, bytes("opaque"));
    rooms_.sends[0].done(41);
    EXPECT_EQ(alice.take(),
              std::vector<std::string>{std::string(R"({"type":"sent","room":")") +
                                       std::string(kRoom) + R"(","id":"first","seq":41})"});
    send(a, "second");
    rooms_.sends[1].done(std::unexpected(RouteError::Unavailable));
    const Seen e = seen(alice.take().at(0));
    EXPECT_EQ(e.reason, "unavailable");
    EXPECT_EQ(e.id, "second");
}

TEST_F(ChatServiceTest, ARateLimitedSendIsRefusedWithWhenToRetryAndNeverReachesTheRoom) {
    FakeClient alice;
    const auto a = attach(alice);
    join(a);
    rooms_.admit();
    alice.take();
    for (int i = 0; i < 10; ++i) {
        send(a, "burst-" + std::to_string(i));
    }
    EXPECT_EQ(rooms_.sends.size(), 10U);
    EXPECT_TRUE(alice.take().empty());

    send(a, "one-too-many");
    EXPECT_EQ(rooms_.sends.size(), 10U);
    const Seen refused = seen(alice.take().at(0));
    EXPECT_EQ(refused.type, "error");
    EXPECT_EQ(refused.reason, "rate_limited");
    EXPECT_EQ(refused.id, "one-too-many");
    EXPECT_EQ(refused.retry_after_ms, 500U);
    EXPECT_EQ(service_->counters().rate_limited, 1U);

    clock_.advance(Millis{499});
    send(a, "still-too-soon");
    EXPECT_EQ(rooms_.sends.size(), 10U);
    clock_.advance(Millis{1});
    send(a, "in-time");
    ASSERT_EQ(rooms_.sends.size(), 11U);
    EXPECT_EQ(rooms_.sends.back().key, key("in-time"));
}

TEST_F(ChatServiceTest, TheSendAllowanceIsTheUsersAcrossConnectionsNotEachConnections) {
    FakeClient phone;
    FakeClient laptop;
    FakeClient other;
    const auto p = attach(phone);
    const auto l = attach(laptop);
    const auto o = attach(other, "bob");
    join(p);
    join(l);
    join(o);
    rooms_.admit();
    for (int i = 0; i < 10; ++i) {
        send(i % 2 == 0 ? p : l, "m" + std::to_string(i));
    }
    laptop.take();
    send(l, "eleventh");
    EXPECT_EQ(seen(laptop.take().at(0)).reason, "rate_limited");
    send(o, "bobs-own");
    EXPECT_EQ(rooms_.sends.size(), 11U);
}

TEST_F(ChatServiceTest, SendingToARoomNotJoinedIsRefusedHere) {
    FakeClient alice;
    const auto a = attach(alice);
    send(a, "x");
    // Joining but not yet joined is not joined either.
    join(a);
    send(a, "y");
    EXPECT_TRUE(rooms_.sends.empty());
    const auto got = alice.take();
    ASSERT_EQ(got.size(), 2U);
    EXPECT_EQ(seen(got[0]).reason, "not_joined");
    EXPECT_EQ(seen(got[1]).id, "y");
    EXPECT_EQ(service_->counters().rate_limited, 0U);
}

TEST_F(ChatServiceTest, SendsInFlightAreBoundedInBytesPerClient) {
    FakeClient alice;
    const auto a = attach(alice);
    join(a);
    rooms_.admit();
    alice.take();
    const std::string big(std::size_t{50} * 1024, 'x');
    send(a, "one", big);
    send(a, "two", big);
    send(a, "three", big);
    EXPECT_EQ(rooms_.sends.size(), 2U);
    EXPECT_EQ(seen(alice.take().at(0)).reason, "busy");
    rooms_.sends[0].done(1);
    alice.take();
    send(a, "four", big);
    EXPECT_EQ(rooms_.sends.size(), 3U);
}

TEST_F(ChatServiceTest, ASendTurnedAwayAsBusyCostsTheClientNoToken) {
    FakeClient alice;
    const auto a = attach(alice);
    join(a);
    rooms_.admit();
    const std::string big(std::size_t{60} * 1024, 'x');
    send(a, "big-1", big);
    send(a, "big-2", big);
    // Refused here, for bytes in flight, before the bucket.
    send(a, "big-3", big);
    EXPECT_EQ(rooms_.sends.size(), 2U);
    // Refused by the owner, for its queue: the token comes back.
    rooms_.sends[0].done(std::unexpected(RouteError::Busy));
    rooms_.sends[1].done(std::unexpected(RouteError::Busy));
    alice.take();
    // Ten tokens were there to start with, and none is spent.
    for (int i = 0; i < 10; ++i) {
        send(a, "small-" + std::to_string(i));
    }
    EXPECT_EQ(rooms_.sends.size(), 12U);
    EXPECT_EQ(service_->counters().rate_limited, 0U);
    send(a, "one-too-many");
    EXPECT_EQ(service_->counters().rate_limited, 1U);
}

TEST_F(ChatServiceTest, AFailedJoinIsReportedAndTheNextJoinAsksAgain) {
    FakeClient alice;
    const auto a = attach(alice);
    join(a);
    rooms_.admit(std::unexpected(RouteError::Unavailable));
    EXPECT_EQ(seen(alice.take().at(0)).reason, "unavailable");
    EXPECT_EQ(service_->rooms(), 0U);
    join(a);
    EXPECT_EQ(rooms_.joins.size(), 1U);
}

constexpr std::size_t kBehind = std::size_t{64} * 1024 + 1;

TEST_F(ChatServiceTest, ALossyClientThatFellBehindIsSentWhatItMissedInOrderOnceItDrains) {
    FakeClient viewer;
    FakeClient member;
    const auto v = attach(viewer);
    const auto m = attach(member, "bob");
    join(v, std::nullopt, chat::Delivery::Lossy);
    join(m);
    rt::IMember& room = rooms_.admit();
    viewer.take();
    member.take();
    deliver(room, 1);
    viewer.unsent = member.unsent = kBehind;
    deliver(room, 2);
    deliver(room, 3);
    // Room again on its socket, but 2 and 3 are still owed: 4 must not overtake them.
    viewer.unsent = member.unsent = 0;
    deliver(room, 4);
    EXPECT_EQ(seqs(viewer.take()), std::vector<std::uint64_t>{1});
    service_->drained(v);
    EXPECT_EQ(seqs(viewer.take()), (std::vector<std::uint64_t>{2, 3, 4}));
    deliver(room, 5);
    EXPECT_EQ(seqs(viewer.take()), std::vector<std::uint64_t>{5});
    // Caught up, a drain sends nothing twice.
    service_->drained(v);
    EXPECT_TRUE(viewer.take().empty());
    // A durable client is never skipped; one too far behind is closed by its connection, and
    // resumes.
    EXPECT_EQ(seqs(member.take()), (std::vector<std::uint64_t>{1, 2, 3, 4, 5}));
    EXPECT_EQ(service_->counters().lossy_drops, 0U);
    EXPECT_EQ(service_->counters().delivered, 10U);
}

TEST_F(ChatServiceTest, AStalledLossyClientIsOwedOnlyTheNewestAndTheOlderAreCountedAsDropped) {
    FakeClient stalled;
    FakeClient keeping_up;
    FakeClient member;
    const auto s = attach(stalled);
    const auto k = attach(keeping_up, "carol");
    const auto m = attach(member, "bob");
    join(s, std::nullopt, chat::Delivery::Lossy);
    join(k, std::nullopt, chat::Delivery::Lossy);
    join(m);
    rt::IMember& room = rooms_.admit();
    for (FakeClient* c : {&stalled, &keeping_up, &member}) {
        c->take();
    }
    stalled.unsent = kBehind;
    constexpr std::uint64_t kSent = 200;
    std::vector<std::uint64_t> all;
    for (std::uint64_t seq = 1; seq <= kSent; ++seq) {
        deliver(room, seq);
        all.push_back(seq);
    }
    EXPECT_TRUE(stalled.take().empty());
    // Dropped as they fall out of the newest 64, not later: a client that never reads again
    // is counted all the same.
    EXPECT_EQ(service_->counters().lossy_drops, kSent - 64);
    EXPECT_EQ(seqs(keeping_up.take()), all);
    EXPECT_EQ(seqs(member.take()), all);

    stalled.unsent = 0;
    service_->drained(s);
    std::vector<std::uint64_t> newest(all.end() - 64, all.end());
    EXPECT_EQ(seqs(stalled.take()), newest);
    EXPECT_EQ(service_->counters().lossy_drops, kSent - 64);
}

TEST_F(ChatServiceTest, CatchingUpStopsWhenTheConnectionIsBehindAgainAndGoesOnAtTheNextDrain) {
    FakeClient viewer;
    const auto v = attach(viewer);
    join(v, std::nullopt, chat::Delivery::Lossy);
    rt::IMember& room = rooms_.admit();
    viewer.take();
    viewer.unsent = kBehind;
    for (std::uint64_t seq = 1; seq <= 5; ++seq) {
        deliver(room, seq);
    }
    // Each message it is sent sits 40 KiB deep on its socket: two of them are past the backlog.
    viewer.growth = std::size_t{40} * 1024;
    for (const std::vector<std::uint64_t>& expected :
         {std::vector<std::uint64_t>{1, 2}, {3, 4}, {5}}) {
        viewer.unsent = 0;
        service_->drained(v);
        EXPECT_EQ(seqs(viewer.take()), expected);
    }
    viewer.unsent = 0;
    deliver(room, 6);
    EXPECT_EQ(seqs(viewer.take()), std::vector<std::uint64_t>{6});
}

class SmallLossyBuffer : public ChatServiceTest {
protected:
    // Four ordinary messages per room ("hi" and its overhead are 258 bytes each).
    SmallLossyBuffer() : ChatServiceTest({.room_buffer_bytes = 4 * 258}) {}
};

TEST_F(SmallLossyBuffer, WhatTheRoomNoLongerKeepsIsDroppedForAClientBehindAsItGoes) {
    FakeClient viewer;
    const auto v = attach(viewer);
    join(v, std::nullopt, chat::Delivery::Lossy);
    rt::IMember& room = rooms_.admit();
    viewer.take();
    viewer.unsent = kBehind;
    for (std::uint64_t seq = 1; seq <= 10; ++seq) {
        deliver(room, seq);
    }
    EXPECT_EQ(service_->counters().lossy_drops, 6U);
    viewer.unsent = 0;
    service_->drained(v);
    EXPECT_EQ(seqs(viewer.take()), (std::vector<std::uint64_t>{7, 8, 9, 10}));
}

TEST_F(ChatServiceTest, JoiningAgainForgetsWhatALossyClientWasOwed) {
    FakeClient viewer;
    const auto v = attach(viewer);
    join(v, std::nullopt, chat::Delivery::Lossy);
    rt::IMember& room = rooms_.admit();
    viewer.take();
    viewer.unsent = kBehind;
    deliver(room, 1);
    deliver(room, 2);
    viewer.unsent = 0;
    join(v);
    EXPECT_EQ(seen(viewer.take().at(0)).type, "joined");
    deliver(room, 3);
    service_->drained(v);
    EXPECT_EQ(seqs(viewer.take()), std::vector<std::uint64_t>{3});
}

TEST_F(ChatServiceTest, EveryViewerOfALiveChatIsLossyWhateverItsJoinAsked) {
    const std::string live = chat::live_chat_room("show-1").to_string();
    FakeClient viewer;
    const auto v = attach(viewer);
    join(v, std::nullopt, chat::Delivery::Durable, live);
    rt::IMember& room = rooms_.admit();
    viewer.take();
    viewer.unsent = kBehind;
    deliver(room, 1, "hi", "bob", live);
    deliver(room, 2, "hi", "bob", live);
    EXPECT_TRUE(viewer.take().empty()) << "a durable client is never held back";
    viewer.unsent = 0;
    service_->drained(v);
    EXPECT_EQ(seqs(viewer.take()), (std::vector<std::uint64_t>{1, 2}));
}

TEST_F(ChatServiceTest, ALiveChatTakesItsAllowanceFromAllSendersHereAndTheRestKeepTheirs) {
    const std::string live = chat::live_chat_room("show-1").to_string();
    std::vector<std::unique_ptr<FakeClient>> clients;
    std::vector<chat::ClientId> ids;
    for (int user = 0; user < 5; ++user) {
        clients.push_back(std::make_unique<FakeClient>());
        ids.push_back(attach(*clients.back(), "user" + std::to_string(user)));
        join(ids.back(), std::nullopt, chat::Delivery::Lossy, live);
        join(ids.back());
    }
    rooms_.admit();
    rooms_.admit();
    // Four users, each within their own ten, use up the room's forty.
    for (std::size_t user = 0; user < 4; ++user) {
        for (int i = 0; i < 10; ++i) {
            send(ids[user], std::format("u{}-{}", user, i), "hi", live);
        }
    }
    EXPECT_EQ(rooms_.sends.size(), 40U);
    clients[4]->take();
    send(ids[4], "late", "hi", live);
    EXPECT_EQ(rooms_.sends.size(), 40U);
    const Seen refused = seen(clients[4]->take().at(0));
    EXPECT_EQ(refused.reason, "rate_limited");
    EXPECT_EQ(refused.retry_after_ms, 50U) << "20 a second";
    EXPECT_EQ(service_->counters().rate_limited, 1U);
    // The room refused it, not the user: their ten are all still theirs elsewhere.
    for (int i = 0; i < 10; ++i) {
        send(ids[4], std::format("elsewhere-{}", i));
    }
    EXPECT_EQ(rooms_.sends.size(), 50U);
    // Half a second gives the user one token back and the room ten.
    clock_.advance(Millis{500});
    send(ids[4], "in-time", "hi", live);
    ASSERT_EQ(rooms_.sends.size(), 51U);
    EXPECT_EQ(rooms_.sends.back().key, key("in-time"));
}

TEST_F(ChatServiceTest, ALiveChatMessageIsALineAndNoLonger) {
    const std::string live = chat::live_chat_room("show-1").to_string();
    FakeClient alice;
    const auto a = attach(alice);
    join(a, std::nullopt, chat::Delivery::Lossy, live);
    join(a);
    rooms_.admit();
    rooms_.admit();
    alice.take();
    send(a, "long", std::string(2'001, 'x'), live);
    EXPECT_TRUE(rooms_.sends.empty());
    const Seen refused = seen(alice.take().at(0));
    EXPECT_EQ(refused.reason, "too_large");
    EXPECT_EQ(refused.id, "long");
    send(a, "line", std::string(2'000, 'x'), live);
    // Other rooms carry up to what the connection decodes.
    send(a, "letter", std::string(20'000, 'x'));
    EXPECT_EQ(rooms_.sends.size(), 2U);
    EXPECT_EQ(service_->counters().rate_limited, 0U);
}

TEST_F(ChatServiceTest, AClientResumingAfterASeqGetsWhatThisNodeKeptSinceInOrder) {
    FakeClient alice;
    FakeClient bob;
    const auto a = attach(alice);
    join(a);
    rt::IMember& room = rooms_.admit();
    for (std::uint64_t seq = 1; seq <= 5; ++seq) {
        deliver(room, seq);
    }
    const auto b = attach(bob, "bob");
    join(b, 2);
    const auto got = bob.take();
    ASSERT_FALSE(got.empty());
    EXPECT_EQ(seen(got[0]).type, "joined");
    EXPECT_EQ(seqs(got), (std::vector<std::uint64_t>{3, 4, 5}));
    deliver(room, 6);
    EXPECT_EQ(seqs(bob.take()), std::vector<std::uint64_t>{6});
    EXPECT_EQ(service_->counters().replayed, 3U);
}

TEST_F(ChatServiceTest, JoinedNamesTheRoomsHeadEvenWhenNothingWasKeptToResumeFrom) {
    FakeClient alice;
    const auto a = attach(alice);
    join(a, 3);
    // The owner has taken seq 7; this node has delivered nothing and kept nothing.
    rt::IMember& room = rooms_.admit(7);
    const auto got = alice.take();
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(seen(got[0]).type, "joined");
    EXPECT_EQ(seen(got[0]).seq, 7U) << "a client at seq 3 must learn it missed 4..7";

    deliver(room, 8);
    FakeClient bob;
    join(attach(bob, "bob"));
    EXPECT_EQ(seen(bob.take().at(0)).seq, 8U);
}

TEST_F(ChatServiceTest, AClientThatLeftComesBackWithinTheLingerAndMissesNothing) {
    FakeClient first;
    const auto before = attach(first);
    join(before);
    rt::IMember& room = rooms_.admit();
    deliver(room, 1);
    service_->detach(before);
    // Nobody here is in the room now; it stays joined, and keeps what arrives.
    deliver(room, 2);
    deliver(room, 3);
    clock_.advance(Millis{29'000});
    service_->sweep();
    EXPECT_TRUE(rooms_.left.empty());

    FakeClient again;
    const auto after = attach(again);
    join(after, 1);
    EXPECT_TRUE(rooms_.joins.empty()) << "the room was still joined";
    EXPECT_EQ(seqs(again.take()), (std::vector<std::uint64_t>{2, 3}));
    EXPECT_TRUE(first.got.size() == 2) << "nothing reached the client once it was detached";
}

TEST_F(ChatServiceTest, ARoomNobodyHereUsesIsLeftOnceTheLingerIsOver) {
    FakeClient alice;
    const auto a = attach(alice);
    join(a);
    rooms_.admit();
    service_->detach(a);
    clock_.advance(Millis{29'999});
    service_->sweep();
    EXPECT_TRUE(rooms_.left.empty());
    clock_.advance(Millis{1'000});
    service_->sweep();
    EXPECT_EQ(rooms_.left, std::vector<core::RoomId>{room_id()});
    EXPECT_EQ(service_->rooms(), 0U);
    EXPECT_EQ(service_->buffered_bytes(), 0U);
}

TEST_F(ChatServiceTest, AResumeQueuesNoMoreThanItsBudgetAndKeepsTheNewest) {
    FakeClient alice;
    FakeClient bob;
    const auto a = attach(alice);
    join(a);
    rt::IMember& room = rooms_.admit();
    // 30 KiB bodies travel as 40 KiB of base64url: three fit a 128 KiB budget, not four.
    const std::string body(std::size_t{30} * 1024, 'r');
    for (std::uint64_t seq = 1; seq <= 5; ++seq) {
        deliver(room, seq, body);
    }
    const auto b = attach(bob, "bob");
    join(b, 0);
    EXPECT_EQ(seqs(bob.take()), (std::vector<std::uint64_t>{3, 4, 5}));
    // With most of the budget already queued on the connection, only the newest fits.
    FakeClient carol;
    carol.unsent = std::size_t{80} * 1024;
    const auto c = attach(carol, "carol");
    join(c, 0);
    EXPECT_EQ(seqs(carol.take()), std::vector<std::uint64_t>{5});
}

TEST_F(ChatServiceTest, ResumingAgainAndAgainReplaysNoMoreThanOneBudgetPerLinger) {
    FakeClient alice;
    FakeClient bob;
    const auto a = attach(alice);
    join(a);
    rt::IMember& room = rooms_.admit();
    // Each 30 KiB body is about 40 KiB on the wire: three fit the 128 KiB budget.
    const std::string body(std::size_t{30} * 1024, 'r');
    for (std::uint64_t seq = 1; seq <= 5; ++seq) {
        deliver(room, seq, body);
    }
    const auto b = attach(bob, "bob");
    join(b, 0);
    EXPECT_EQ(seqs(bob.take()), (std::vector<std::uint64_t>{3, 4, 5}));
    // bob has read everything, and asks again: this linger's budget is spent.
    join(b, 0);
    EXPECT_TRUE(seqs(bob.take()).empty());
    clock_.advance(Millis{30'000});
    join(b, 0);
    EXPECT_EQ(seqs(bob.take()), (std::vector<std::uint64_t>{3, 4, 5}));
}

class TwoJoins : public ChatServiceTest {
protected:
    TwoJoins() : ChatServiceTest({.join_burst = 2}) {}
};

TEST_F(TwoJoins, AResumeInARoomAlreadyJoinedCostsAJoinAndAPlainRejoinDoesNot) {
    FakeClient alice;
    const auto a = attach(alice);
    join(a);
    rooms_.admit();
    join(a);
    join(a);
    join(a, 0);
    join(a, 0);
    const auto got = alice.take();
    ASSERT_EQ(got.size(), 5U);
    EXPECT_EQ(seen(got[3]).type, "joined");
    EXPECT_EQ(seen(got[4]).reason, "busy");
}

class SmallBuffers : public ChatServiceTest {
protected:
    // Two ordinary messages per room, and three across rooms.
    SmallBuffers()
        : ChatServiceTest({.room_buffer_bytes = 600, .buffer_bytes = 900, .buffer_messages = 100}) {
    }
};

TEST_F(SmallBuffers, ARoomKeepsItsNewestMessagesAndAllRoomsTogetherDropTheOldestFirst) {
    FakeClient alice;
    const auto a = attach(alice);
    join(a);
    join(a, std::nullopt, chat::Delivery::Durable, kOtherRoom);
    rt::IMember& first = rooms_.admit();
    rt::IMember& second = rooms_.admit();
    for (std::uint64_t seq = 1; seq <= 3; ++seq) {
        deliver(first, seq);
    }
    EXPECT_EQ(service_->buffered_bytes(), 2 * (2 + 256U));
    deliver(second, 1, "hi", "bob", kOtherRoom);
    deliver(second, 2, "hi", "bob", kOtherRoom);
    // Four kept is past 900 bytes: the oldest of all, the first room's seq 2, went.
    EXPECT_EQ(service_->buffered_bytes(), 3 * (2 + 256U));
    alice.take();
    join(a, 0);
    EXPECT_EQ(seqs(alice.take()), std::vector<std::uint64_t>{3});
    join(a, 0, chat::Delivery::Durable, kOtherRoom);
    EXPECT_EQ(seqs(alice.take()), (std::vector<std::uint64_t>{1, 2}));
}

TEST_F(ChatServiceTest, ADetachedClientIsToldNothingMoreEvenOfItsOwnSends) {
    FakeClient alice;
    FakeClient bob;
    const auto a = attach(alice);
    const auto b = attach(bob, "bob");
    join(a);
    join(b);
    rt::IMember& room = rooms_.admit();
    send(a, "late");
    alice.take();
    service_->detach(a);
    rooms_.sends[0].done(1);
    deliver(room, 1);
    EXPECT_TRUE(alice.got.empty());
    EXPECT_EQ(seqs(bob.take()), std::vector<std::uint64_t>{1});
}

TEST_F(ChatServiceTest, ClientsJoinAtMostTheirShareOfRoomsAndUsersAtMostTheirRate) {
    FakeClient alice;
    const auto a = attach(alice);
    std::string room(kRoom);
    for (int i = 0; i < 65; ++i) {
        room.replace(room.size() - 2, 2, std::format("{:02x}", i));
        join(a, std::nullopt, chat::Delivery::Durable, room);
    }
    EXPECT_EQ(rooms_.joins.size(), 64U);
    EXPECT_EQ(seen(alice.take().back()).reason, "too_many_rooms");

    // A second connection of the same user has used up nothing of its own rooms, but the
    // user's joins are spent: 64 at once, then one a second.
    FakeClient again;
    const auto b = attach(again);
    join(b, std::nullopt, chat::Delivery::Durable, kOtherRoom);
    EXPECT_EQ(seen(again.take().back()).reason, "busy");
    clock_.advance(Millis{1'000});
    join(b, std::nullopt, chat::Delivery::Durable, kOtherRoom);
    EXPECT_EQ(rooms_.joins.size(), 65U);
}

} // namespace
