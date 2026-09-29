#include "core/util/json.hpp"
#include "infra/auth/base64url.hpp"

#include "chat_service.hpp"
#include "support/fake_clock.hpp"

#include <algorithm>
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

// Member lists, answered at once by default. The port never answers from inside a call; the
// service copes with either, and tests that do not care about membership read simpler this way.
// With `hold` set, answers wait for answer_admits().
class FakeMessages final : public core::ports::IMessageStore {
public:
    void history_before(
        const core::RoomId& /*room*/, std::optional<std::uint64_t> /*before*/,
        std::size_t /*limit*/,
        core::ports::MessageCallback<std::vector<core::ports::StoredMessage>> done) override {
        directions.emplace_back("before");
        pages.push_back(std::move(done));
    }
    void history_after(
        const core::RoomId& /*room*/, std::uint64_t /*after*/, std::size_t /*limit*/,
        core::ports::MessageCallback<std::vector<core::ports::StoredMessage>> done) override {
        directions.emplace_back("after");
        pages.push_back(std::move(done));
    }
    void last_seq(const core::RoomId& /*room*/,
                  core::ports::MessageCallback<std::uint64_t> done) override {
        done(std::uint64_t{0});
    }
    void add_member(const core::RoomId& /*room*/, const core::UserId& /*user*/,
                    core::ports::MessageCallback<void> done) override {
        done({});
    }
    void remove_member(const core::RoomId& /*room*/, const core::UserId& /*user*/,
                       core::ports::MessageCallback<void> done) override {
        done({});
    }
    void members(const core::RoomId& /*room*/, std::optional<core::UserId> /*after*/,
                 std::size_t /*limit*/,
                 core::ports::MessageCallback<std::vector<core::UserId>> done) override {
        done(std::vector<core::UserId>{});
    }
    void admits(const core::RoomId& /*room*/, const core::UserId& user, core::ports::RoomKind asked,
                core::ports::MessageCallback<bool> done) override {
        kinds.push_back(asked);
        const bool admitted = std::ranges::find(refused, user.view()) == refused.end();
        if (hold) {
            held.emplace_back(std::move(done), admitted);
            return;
        }
        done(admitted);
    }

    void answer_admits(core::ports::MessageResult<bool> result) {
        auto [done, admitted] = std::move(held.front());
        held.erase(held.begin());
        done(result ? core::ports::MessageResult<bool>{admitted} : result);
    }

    std::vector<std::string> refused;
    std::vector<core::ports::RoomKind> kinds;
    bool hold = false;
    std::vector<std::pair<core::ports::MessageCallback<bool>, bool>> held;
    std::vector<core::ports::MessageCallback<std::vector<core::ports::StoredMessage>>> pages;
    std::vector<std::string> directions;
};

class FakeClient final : public chat::IClient {
public:
    bool push(std::string_view text) noexcept override {
        if (closing) {
            return false;
        }
        got.emplace_back(text);
        return true;
    }
    [[nodiscard]] std::size_t unsent_bytes() const noexcept override { return unsent; }
    void allocation_failed() noexcept override { ++failures; }

    // What arrived since the last call.
    std::vector<std::string> take() { return std::exchange(got, {}); }

    std::vector<std::string> got;
    std::size_t unsent = 0;
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
        : service_(std::make_unique<chat::ChatService>(rooms_, messages_, clock_, limits)) {}

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

    void history(chat::ClientId id, std::optional<std::uint64_t> before = std::nullopt,
                 std::string_view room = kRoom) {
        service_->history(id, {.room = room_id(room), .before = before, .after = std::nullopt});
    }

    static core::ports::StoredMessage stored(std::uint64_t seq, std::string_view body) {
        return {.seq = seq,
                .sender = *core::UserId::parse("bob"),
                .key = "m" + std::to_string(seq),
                .sent_at = core::WallTime{},
                .body = bytes(body)};
    }

    FakeRooms rooms_;
    FakeMessages messages_;
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

TEST_F(ChatServiceTest, AJoinOfARoomThatDoesNotAdmitTheUserIsRefusedAndNeverReachesTheRoom) {
    messages_.refused = {"mallory"};
    FakeClient mallory;
    const auto m = attach(mallory, "mallory");
    join(m);
    EXPECT_TRUE(rooms_.joins.empty());
    const Seen refused = seen(mallory.take().at(0));
    EXPECT_EQ(refused.type, "error");
    EXPECT_EQ(refused.reason, "not_member");
    // Not joined, so nothing can be sent there either.
    send(m, "try");
    EXPECT_TRUE(rooms_.sends.empty());
    EXPECT_EQ(seen(mallory.take().at(0)).reason, "not_joined");
}

TEST_F(ChatServiceTest, TheKindAJoinNamesIsWhatTheMemberCheckIsAskedFor) {
    FakeClient alice;
    const auto a = attach(alice);
    join(a);
    service_->join(a, {.room = room_id(kOtherRoom),
                       .after = std::nullopt,
                       .delivery = chat::Delivery::Lossy,
                       .kind = core::ports::RoomKind::StreamLiveChat});
    EXPECT_EQ(messages_.kinds, (std::vector{core::ports::RoomKind::GroupChat,
                                            core::ports::RoomKind::StreamLiveChat}));
}

TEST_F(ChatServiceTest, AJoinWaitsForTheMemberListAndAskingAgainMeanwhileIsBusy) {
    messages_.hold = true;
    FakeClient alice;
    const auto a = attach(alice);
    join(a);
    join(a);
    EXPECT_TRUE(rooms_.joins.empty());
    EXPECT_EQ(seen(alice.take().at(0)).reason, "busy");
    messages_.answer_admits(true);
    ASSERT_EQ(rooms_.joins.size(), 1U);
    rooms_.admit(7);
    const Seen joined = seen(alice.take().at(0));
    EXPECT_EQ(joined.type, "joined");
    EXPECT_EQ(joined.seq, 7U);
}

TEST_F(ChatServiceTest, AMemberListThatCannotBeReadRefusesTheJoinAsUnavailable) {
    messages_.hold = true;
    FakeClient alice;
    const auto a = attach(alice);
    join(a);
    messages_.answer_admits(std::unexpected(core::ports::MessageStoreError::Unavailable));
    EXPECT_TRUE(rooms_.joins.empty());
    EXPECT_EQ(seen(alice.take().at(0)).reason, "unavailable");
    // The client is not left half in the room: asking again asks the member list again.
    join(a);
    ASSERT_EQ(messages_.held.size(), 1U);
}

TEST_F(ChatServiceTest, AnAnswerForAClientThatLeftMeanwhileIsDropped) {
    messages_.hold = true;
    FakeClient alice;
    const auto a = attach(alice);
    join(a);
    service_->detach(a);
    messages_.answer_admits(true);
    EXPECT_TRUE(rooms_.joins.empty());
    EXPECT_TRUE(alice.take().empty());
}

TEST_F(ChatServiceTest, AHistoryPageArrivesAsItsMessagesInTheStoresOrderThenItsCount) {
    FakeClient alice;
    const auto a = attach(alice);
    join(a);
    rooms_.admit(9);
    alice.take();
    history(a, 9);
    ASSERT_EQ(messages_.pages.size(), 1U);
    messages_.pages[0](std::vector{stored(8, "later"), stored(7, "earlier")});
    const auto got = alice.take();
    ASSERT_EQ(got.size(), 3U);
    EXPECT_EQ(seen(got[0]).seq, 8U);
    EXPECT_EQ(seen(got[0]).body, "later");
    EXPECT_EQ(seen(got[0]).id, "m8");
    EXPECT_EQ(seen(got[1]).seq, 7U);
    EXPECT_EQ(got[2], std::string(R"({"type":"history","room":")") + std::string(kRoom) +
                          R"(","count":2})");
    EXPECT_EQ(service_->counters().history_messages, 2U);
}

TEST_F(ChatServiceTest, AfterReadsForwardFromTheCursorAndBeforeOrNoCursorBackward) {
    FakeClient alice;
    const auto a = attach(alice);
    join(a);
    rooms_.admit(9);
    service_->history(a, {.room = room_id(), .before = std::nullopt, .after = 3});
    history(a, 9);
    history(a);
    EXPECT_EQ(messages_.directions, (std::vector<std::string>{"after", "before", "before"}));
}

TEST_F(ChatServiceTest, HistoryIsRefusedWhileTheMemberCheckIsStillOut) {
    messages_.hold = true;
    FakeClient alice;
    const auto a = attach(alice);
    join(a);
    alice.take();
    history(a);
    EXPECT_TRUE(messages_.pages.empty());
    EXPECT_EQ(seen(alice.take().at(0)).reason, "not_joined");
}

TEST_F(ChatServiceTest, HistoryOfARoomNotJoinedIsRefusedWithoutReadingTheStore) {
    FakeClient alice;
    const auto a = attach(alice);
    history(a);
    EXPECT_TRUE(messages_.pages.empty());
    EXPECT_EQ(seen(alice.take().at(0)).reason, "not_joined");
}

TEST_F(ChatServiceTest, AHistoryPageIsCutToWhatTheClientCanStillTakeButNeverToNothing) {
    FakeClient alice;
    const auto a = attach(alice);
    join(a);
    rooms_.admit(2);
    alice.take();
    const std::string body(1000, 'x');
    const std::size_t one = chat::message_wire_size(body.size());
    // Room for one message and a little more, not two.
    alice.unsent = chat::ServiceLimits{}.replay_budget - one - 10;
    history(a);
    messages_.pages[0](std::vector{stored(2, body), stored(1, body)});
    const auto got = alice.take();
    ASSERT_EQ(got.size(), 2U);
    EXPECT_EQ(seen(got[0]).seq, 2U);
    EXPECT_EQ(got[1], std::string(R"({"type":"history","room":")") + std::string(kRoom) +
                          R"(","count":1})");

    alice.unsent = chat::ServiceLimits{}.replay_budget - 10;
    history(a, 2);
    messages_.pages[1](std::vector{stored(1, body)});
    EXPECT_EQ(seen(alice.take().at(0)).reason, "busy");
}

TEST_F(ChatServiceTest, AnEmptyPageIsTheStartOfTheRoomAndAStoreThatFailsIsUnavailable) {
    FakeClient alice;
    const auto a = attach(alice);
    join(a);
    rooms_.admit(0);
    alice.take();
    history(a);
    messages_.pages[0](std::vector<core::ports::StoredMessage>{});
    EXPECT_EQ(alice.take(), std::vector<std::string>{std::string(R"({"type":"history","room":")") +
                                                     std::string(kRoom) + R"(","count":0})"});
    history(a);
    messages_.pages[1](std::unexpected(core::ports::MessageStoreError::Unavailable));
    EXPECT_EQ(seen(alice.take().at(0)).reason, "unavailable");
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
    // The id was already used for another body: the client learns it, and the id is spent.
    send(a, "first", "something else");
    rooms_.sends[2].done(std::unexpected(RouteError::Conflict));
    const Seen c = seen(alice.take().at(0));
    EXPECT_EQ(c.reason, "conflict");
    EXPECT_EQ(c.id, "first");
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

TEST_F(ChatServiceTest, AClientBehindInALossyRoomIsSkippedAndCountedWhileADurableOneIsNot) {
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
    viewer.unsent = member.unsent = std::size_t{64} * 1024 + 1;
    deliver(room, 2);
    viewer.unsent = member.unsent = 0;
    deliver(room, 3);
    EXPECT_EQ(seqs(viewer.take()), (std::vector<std::uint64_t>{1, 3}));
    // A durable client is never skipped; one too far behind is closed by its connection, and
    // resumes.
    EXPECT_EQ(seqs(member.take()), (std::vector<std::uint64_t>{1, 2, 3}));
    EXPECT_EQ(service_->counters().lossy_drops, 1U);
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

TEST_F(TwoJoins, APageOfHistoryCostsAJoin) {
    FakeClient alice;
    const auto a = attach(alice);
    join(a);
    rooms_.admit();
    alice.take();
    history(a);
    history(a);
    EXPECT_EQ(messages_.pages.size(), 1U);
    EXPECT_EQ(seen(alice.take().at(0)).reason, "busy");
    clock_.advance(Millis{1'000});
    history(a);
    EXPECT_EQ(messages_.pages.size(), 2U);
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
