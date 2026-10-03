#include "core/util/json.hpp"
#include "infra/auth/base64url.hpp"

#include "call.hpp"
#include "chat_service.hpp"
#include "support/fake_clock.hpp"

#include <algorithm>
#include <array>
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
// The stream show-1's live chat (tests/unit/chat/live_chat_test.cpp).
constexpr std::string_view kLiveRoom = "011b9ed0-d6b6-88e6-ac34-32d7070ba83b";

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

    void ask_owner(const core::RoomId& room, rt::IMember& from, std::span<const std::byte> request,
                   rt::OwnerAnswer done) override {
        asks.push_back({.room = room,
                        .member = &from,
                        .request = {request.begin(), request.end()},
                        .done = std::move(done)});
    }

    struct Asking {
        core::RoomId room;
        rt::IMember* member;
        std::vector<std::byte> request;
        rt::OwnerAnswer done;
    };

    // Answers the oldest join, and returns the member it made.
    rt::IMember& admit(std::expected<std::uint64_t, RouteError> result = 0) {
        Joining j = std::move(joins.front());
        joins.erase(joins.begin());
        j.done(result);
        return *j.member;
    }

    std::vector<Joining> joins;
    std::vector<Sending> sends;
    std::vector<Asking> asks;
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
    using core::ports::IMessageStore::admits;
    void admits(const core::RoomId& room, const core::UserId& user, core::ports::RoomKind asked,
                core::ports::Recording recording,
                core::ports::MessageCallback<core::ports::Admission> done) override {
        kinds.push_back(asked);
        recordings.push_back(recording);
        asked_rooms.push_back(room);
        auto answer = core::ports::Admission::Admitted;
        if (std::ranges::find(refused, user.view()) != refused.end()) {
            answer = core::ports::Admission::NotMember;
        } else if (!live && core::ports::admits_anyone(asked)) {
            answer = core::ports::Admission::NotLive;
        }
        if (hold) {
            held.emplace_back(std::move(done), answer);
            return;
        }
        done(answer);
    }
    void access(const core::RoomId& /*room*/, const core::UserId& user,
                core::ports::MessageCallback<core::ports::RoomAccess> done) override {
        done(core::ports::RoomAccess{.kind = core::ports::RoomKind::DirectChat,
                                     .member =
                                         std::ranges::find(refused, user.view()) == refused.end()});
    }
    void record_live(const core::RoomId& /*room*/,
                     core::ports::MessageCallback<void> done) override {
        done({});
    }
    void watch_members(core::ports::IMemberListener* listener) noexcept override {
        watcher = listener;
    }

    // Answers the oldest held admits with what it would have answered, or with `result`'s error.
    void answer_admits(core::ports::MessageResult<void> result) {
        auto [done, answer] = std::move(held.front());
        held.erase(held.begin());
        if (result) {
            done(answer);
        } else {
            done(std::unexpected(result.error()));
        }
    }

    std::vector<std::string> refused;
    core::ports::IMemberListener* watcher = nullptr;
    // Whether every room is recorded as live; otherwise a join that asks for live is NotLive.
    bool live = true;
    std::vector<core::ports::RoomKind> kinds;
    std::vector<core::ports::Recording> recordings;
    std::vector<core::RoomId> asked_rooms;
    bool hold = false;
    std::vector<
        std::pair<core::ports::MessageCallback<core::ports::Admission>, core::ports::Admission>>
        held;
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

// Each refused join may have recorded a room that nobody uses (ADR-0054's kind record): past the
// user's allowance a join is answered the same and records nothing, until the allowance refills.
TEST_F(ChatServiceTest, RefusedJoinsRecordRoomsOnlyWithinTheUsersAllowance) {
    messages_.refused = {"mallory"};
    FakeClient mallory;
    const auto m = attach(mallory, "mallory");
    const chat::ServiceLimits limits;
    const auto join_room = [&](std::uint32_t n) {
        const std::string room = std::format("01a0eb86-6cca-7dce-84cc-{:012x}", n);
        join(m, std::nullopt, chat::Delivery::Durable, room);
        return seen(mallory.take().at(0)).reason;
    };
    std::uint32_t n = 0;
    for (; n < limits.record_burst; ++n) {
        EXPECT_EQ(join_room(n), "not_member");
        // One a second: within the join limit, and so that joins are never what refuses them.
        clock_.advance(std::chrono::seconds(1));
    }
    EXPECT_EQ(std::ranges::count(messages_.recordings, core::ports::Recording::Allowed),
              limits.record_burst);
    // Answered as before, but nothing recorded.
    EXPECT_EQ(join_room(n++), "not_member");
    EXPECT_EQ(messages_.recordings.back(), core::ports::Recording::Skipped);
    EXPECT_EQ(service_->counters().unrecorded_joins, 1U);
    // One more a record_interval.
    clock_.advance(limits.record_interval);
    EXPECT_EQ(join_room(n++), "not_member");
    EXPECT_EQ(messages_.recordings.back(), core::ports::Recording::Allowed);
    EXPECT_EQ(join_room(n++), "not_member");
    EXPECT_EQ(messages_.recordings.back(), core::ports::Recording::Skipped);

    // Another user's joins, admitted, are never charged.
    FakeClient alice;
    const auto a = attach(alice);
    for (std::uint32_t i = 0; i < limits.record_burst + 5; ++i) {
        join(a, std::nullopt, chat::Delivery::Durable,
             std::format("01a0eb86-6cca-7dce-84cd-{:012x}", i));
        clock_.advance(std::chrono::seconds(1));
    }
    EXPECT_EQ(messages_.recordings.back(), core::ports::Recording::Allowed);
}

// Membership is checked at the join; a user taken off the list afterwards must stop hearing the
// room at once, not when the connection closes (ADR-0073).
TEST_F(ChatServiceTest, AUserRemovedFromTheMemberListLeavesTheRoomAndHearsNothingMore) {
    ASSERT_NE(messages_.watcher, nullptr) << "the service does not listen for removals";
    FakeClient alice;
    FakeClient bob;
    FakeClient bob_phone;
    const auto a = attach(alice);
    const auto b = attach(bob, "bob");
    const auto p = attach(bob_phone, "bob");
    join(a);
    join(b);
    join(p);
    rt::IMember& member = rooms_.admit();
    alice.take();
    bob.take();
    bob_phone.take();

    messages_.watcher->on_member_removed(room_id(), *core::UserId::parse("bob"));
    for (FakeClient* removed : {&bob, &bob_phone}) {
        const auto got = removed->take();
        ASSERT_EQ(got.size(), 1U);
        EXPECT_EQ(seen(got[0]).type, "error");
        EXPECT_EQ(seen(got[0]).reason, "not_member");
    }
    EXPECT_TRUE(alice.take().empty());
    EXPECT_EQ(service_->counters().removals, 2U);

    deliver(member, 1, "after");
    EXPECT_EQ(seen(alice.take().at(0)).body, "after");
    EXPECT_TRUE(bob.take().empty());
    EXPECT_TRUE(bob_phone.take().empty());
    // Nor may it send there, or read the room's history.
    send(b, "still-here");
    EXPECT_TRUE(rooms_.sends.empty());
    EXPECT_EQ(seen(bob.take().at(0)).reason, "not_joined");
    history(b);
    EXPECT_EQ(seen(bob.take().at(0)).reason, "not_joined");
}

// The member list may have been read before the removal committed: a join still waiting for it
// is refused whatever it says.
TEST_F(ChatServiceTest, AJoinWaitingForTheMemberListWhenItsUserIsRemovedIsRefused) {
    messages_.hold = true;
    FakeClient bob;
    const auto b = attach(bob, "bob");
    join(b);
    messages_.watcher->on_member_removed(room_id(), *core::UserId::parse("bob"));
    messages_.answer_admits({});
    EXPECT_TRUE(rooms_.joins.empty());
    EXPECT_EQ(seen(bob.take().at(0)).reason, "not_member");
    // Asking again asks the member list again.
    join(b);
    ASSERT_EQ(messages_.held.size(), 1U);
}

// Removals said while the store was not listening are lost: a resync checks every closed room a
// client here is in against its member list again.
TEST_F(ChatServiceTest, AfterAResyncAClientNoLongerOnTheListLeavesTheRoom) {
    FakeClient alice;
    FakeClient bob;
    const auto a = attach(alice);
    const auto b = attach(bob, "bob");
    join(a);
    join(b);
    rooms_.admit();
    alice.take();
    bob.take();
    messages_.refused = {"bob"};
    messages_.watcher->on_members_resync();
    EXPECT_EQ(seen(bob.take().at(0)).reason, "not_member");
    EXPECT_TRUE(alice.take().empty());
    EXPECT_EQ(service_->counters().removals, 1U);
}

// A resync comes after the store lost its way to the database, so its checks may fail too: a
// removal they would have found must not be lost with them. They are asked again once the
// store answers, paced so that a store still down is not asked again at once.
TEST_F(ChatServiceTest, AResyncWhoseChecksFailAsksAgainAndStillFindsTheRemoval) {
    FakeClient alice;
    FakeClient bob;
    const auto a = attach(alice);
    const auto b = attach(bob, "bob");
    join(a);
    join(b);
    rooms_.admit();
    alice.take();
    bob.take();
    messages_.hold = true;
    messages_.refused = {"bob"};
    messages_.watcher->on_members_resync();
    ASSERT_EQ(messages_.held.size(), 2U);
    while (!messages_.held.empty()) {
        messages_.answer_admits(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
    EXPECT_TRUE(bob.take().empty());
    // Not at once: the store that just failed is not asked again in the same breath.
    service_->sweep();
    EXPECT_TRUE(messages_.held.empty());

    clock_.advance(Millis{1'000});
    service_->sweep();
    ASSERT_EQ(messages_.held.size(), 2U) << "the failed checks were dropped";
    while (!messages_.held.empty()) {
        messages_.answer_admits({});
    }
    EXPECT_EQ(seen(bob.take().at(0)).reason, "not_member");
    EXPECT_TRUE(alice.take().empty());
    EXPECT_EQ(service_->counters().removals, 1U);
}

// A node holds up to 1280 connections of 64 rooms each: a resync asks about each room and user
// once, however many of the user's clients are in it, and no more at a time than the store has
// connections, so that joins and history reads are not queued behind tens of thousands.
TEST_F(ChatServiceTest, AResyncAsksAboutEachRoomAndUserOnceAndOnlyAFewAtATime) {
    constexpr int kRooms = 6;
    const auto room = [](int i) { return std::format("01a0eb86-6cca-7dce-84cc-3bb47615f90{}", i); };
    FakeClient phone;
    FakeClient laptop;
    const auto p = attach(phone, "bob");
    const auto l = attach(laptop, "bob");
    for (int i = 0; i < kRooms; ++i) {
        join(p, std::nullopt, chat::Delivery::Durable, room(i));
        join(l, std::nullopt, chat::Delivery::Durable, room(i));
        rooms_.admit();
    }
    const std::size_t asked_before = messages_.kinds.size();
    messages_.hold = true;
    messages_.watcher->on_members_resync();
    EXPECT_EQ(messages_.held.size(), 4U);
    std::size_t most = messages_.held.size();
    while (!messages_.held.empty()) {
        messages_.answer_admits({});
        most = std::max(most, messages_.held.size());
    }
    EXPECT_LE(most, 4U);
    EXPECT_EQ(messages_.kinds.size() - asked_before, std::size_t{kRooms});
}

// A listening session that flaps faster than the checks drain must not re-ask the same rooms
// each time and never reach the rest: what a resync queued and has not asked stays queued.
TEST_F(ChatServiceTest, ResyncsInARowStillCheckEveryRoom) {
    constexpr int kRooms = 10;
    const auto room = [](int i) {
        return std::format("01a0eb86-6cca-7dce-84cc-3bb47615f9{:02}", i);
    };
    FakeClient bob;
    const auto b = attach(bob, "bob");
    for (int i = 0; i < kRooms; ++i) {
        join(b, std::nullopt, chat::Delivery::Durable, room(i));
        rooms_.admit();
    }
    messages_.hold = true;
    const std::size_t before = messages_.asked_rooms.size();
    for (int round = 0; round < 3; ++round) {
        messages_.watcher->on_members_resync();
        for (int i = 0; i < 4 && !messages_.held.empty(); ++i) {
            messages_.answer_admits({});
        }
    }
    const std::vector<core::RoomId> asked(messages_.asked_rooms.begin() +
                                              static_cast<std::ptrdiff_t>(before),
                                          messages_.asked_rooms.end());
    for (int i = 0; i < kRooms; ++i) {
        EXPECT_NE(std::ranges::find(asked, room_id(room(i))), asked.end())
            << room(i) << " was never checked";
    }
}

// A check that fails for a reason other than the store being unreachable will fail the same
// way again: it is not retried for ever, pausing every other check each time, but settled on
// the safe side, the user out of the room.
TEST_F(ChatServiceTest, ACheckThatFailsForGoodTakesTheUserOutOfTheRoom) {
    FakeClient bob;
    const auto b = attach(bob, "bob");
    join(b);
    rooms_.admit();
    bob.take();
    messages_.hold = true;
    messages_.watcher->on_members_resync();
    ASSERT_EQ(messages_.held.size(), 1U);
    messages_.answer_admits(std::unexpected(core::ports::MessageStoreError::Corrupt));
    // Not not_member, which says the list left the user out: the list was never read, and a
    // join asks it again.
    EXPECT_EQ(seen(bob.take().at(0)).reason, "unavailable");
    EXPECT_EQ(service_->counters().failed_rechecks, 1U);
    clock_.advance(Millis{1'000});
    service_->sweep();
    EXPECT_TRUE(messages_.held.empty()) << "asked again";
}

// Shutdown destroys the message store before the server, whose destructor still sweeps the
// service: once stopped, the service must not call the store, whatever a resync left queued.
TEST_F(ChatServiceTest, AStoppedServiceAsksTheStoreNothingMoreThoughChecksWereQueued) {
    constexpr int kRooms = 6;
    const auto room = [](int i) { return std::format("01a0eb86-6cca-7dce-84cc-3bb47615f90{}", i); };
    FakeClient bob;
    const auto b = attach(bob, "bob");
    for (int i = 0; i < kRooms; ++i) {
        join(b, std::nullopt, chat::Delivery::Durable, room(i));
        rooms_.admit();
    }
    messages_.hold = true;
    messages_.watcher->on_members_resync();
    ASSERT_EQ(messages_.held.size(), 4U);
    // The store is down: the checks fail, and wait a second.
    while (!messages_.held.empty()) {
        messages_.answer_admits(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
    const std::size_t asked = messages_.asked_rooms.size();
    service_->stop();
    clock_.advance(Millis{1'000});
    service_->sweep();
    messages_.watcher->on_members_resync();
    EXPECT_EQ(messages_.asked_rooms.size(), asked) << "the store was called after stop";
}

// A join whose member list was read before a removal that went unannounced is let in after the
// resync looked over the rooms: the resync must ask about it again once it is in.
TEST_F(ChatServiceTest, AJoinStillWaitingForTheMemberListDuringAResyncIsCheckedOnceAdmitted) {
    FakeClient bob;
    const auto b = attach(bob, "bob");
    messages_.hold = true;
    join(b);
    ASSERT_EQ(messages_.held.size(), 1U);
    // Removed after the join's read, while the store was not listening.
    messages_.refused = {"bob"};
    messages_.watcher->on_members_resync();
    EXPECT_TRUE(messages_.held.size() == 1U);
    messages_.answer_admits({});
    rooms_.admit();
    EXPECT_EQ(seen(bob.take().at(0)).type, "joined");
    ASSERT_EQ(messages_.held.size(), 1U) << "the room the resync skipped is never checked";
    messages_.answer_admits({});
    EXPECT_EQ(seen(bob.take().at(0)).reason, "not_member");
    EXPECT_EQ(service_->counters().removals, 1U);
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

TEST_F(ChatServiceTest, AJoinThatAsksForLiveInARoomNotRecordedLiveIsRefusedAsNotLive) {
    messages_.live = false;
    FakeClient alice;
    const auto a = attach(alice);
    service_->join(a, {.room = room_id(kOtherRoom),
                       .after = std::nullopt,
                       .delivery = chat::Delivery::Lossy,
                       .kind = core::ports::RoomKind::StreamLiveChat});
    EXPECT_TRUE(rooms_.joins.empty());
    EXPECT_EQ(seen(alice.take().at(0)).reason, "not_live");
    // Not left half in the room: a join as a group chat asks again, and is let in.
    join(a);
    EXPECT_EQ(rooms_.joins.size(), 1U);
}

TEST_F(ChatServiceTest, AJoinWaitsForTheMemberListAndAskingAgainMeanwhileIsBusy) {
    messages_.hold = true;
    FakeClient alice;
    const auto a = attach(alice);
    join(a);
    join(a);
    EXPECT_TRUE(rooms_.joins.empty());
    EXPECT_EQ(seen(alice.take().at(0)).reason, "busy");
    messages_.answer_admits({});
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
    messages_.answer_admits({});
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

// Only the owner's load gives a token back. A send that failed for any other reason reached the
// owner, so it spent the token as a sent one would.
TEST_F(ChatServiceTest, ASendThatFailsForAnyReasonButLoadStillCostsItsToken) {
    FakeClient alice;
    const auto a = attach(alice);
    join(a);
    rooms_.admit();
    const std::array failures{RouteError::NotJoined, RouteError::Fenced, RouteError::Unavailable,
                              RouteError::Conflict};
    for (std::size_t i = 0; i < 10; ++i) {
        send(a, "m" + std::to_string(i));
        rooms_.sends.back().done(std::unexpected(failures.at(i % failures.size())));
    }
    alice.take();
    send(a, "one-too-many");
    EXPECT_EQ(rooms_.sends.size(), 10U);
    EXPECT_EQ(seen(alice.take().at(0)).reason, "rate_limited");
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

constexpr std::size_t kBehind = (std::size_t{64} * 1024) + 1;

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
    const std::vector<std::uint64_t> newest(all.end() - 64, all.end());
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
    SmallLossyBuffer() : ChatServiceTest({.room_buffer_bytes = std::size_t{4} * 258}) {}
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

TEST_F(ChatServiceTest, JoiningAgainForgetsWhatALossyClientWasOwedAndCountsItDropped) {
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
    EXPECT_EQ(service_->counters().lossy_drops, 2U);
    deliver(room, 3);
    service_->drained(v);
    EXPECT_EQ(seqs(viewer.take()), std::vector<std::uint64_t>{3});
    EXPECT_EQ(service_->counters().lossy_drops, 2U);
}

// Rejoining with a resume sends again what it names from the node's kept messages: those are
// not dropped, only what the resume leaves out.
TEST_F(ChatServiceTest, ARejoinThatResumesCountsDroppedOnlyWhatItsResumeLeavesOut) {
    FakeClient viewer;
    FakeClient other;
    const auto v = attach(viewer);
    const auto o = attach(other, "bob");
    join(v, std::nullopt, chat::Delivery::Lossy);
    join(o, std::nullopt, chat::Delivery::Lossy);
    rt::IMember& room = rooms_.admit();
    for (std::uint64_t seq = 1; seq <= 499; ++seq) {
        deliver(room, seq);
    }
    // Behind from 500 up to the head, 563: owed 64, the most a lossy client is owed.
    viewer.unsent = other.unsent = kBehind;
    for (std::uint64_t seq = 500; seq <= 563; ++seq) {
        deliver(room, seq);
    }
    viewer.take();
    other.take();
    viewer.unsent = other.unsent = 0;
    join(v, 499, chat::Delivery::Lossy);
    std::vector<std::uint64_t> owed;
    for (std::uint64_t seq = 500; seq <= 563; ++seq) {
        owed.push_back(seq);
    }
    EXPECT_EQ(seqs(viewer.take()), owed);
    EXPECT_EQ(service_->counters().lossy_drops, 0U);
    // A resume from later leaves 500..530 out, and only those are dropped.
    join(o, 530, chat::Delivery::Lossy);
    EXPECT_EQ(seqs(other.take()), std::vector<std::uint64_t>(owed.begin() + 31, owed.end()));
    EXPECT_EQ(service_->counters().lossy_drops, 31U);
}

TEST_F(ChatServiceTest, AClientThatLeavesBeforeItCaughtUpHasTheRestCountedDropped) {
    FakeClient viewer;
    const auto v = attach(viewer);
    join(v, std::nullopt, chat::Delivery::Lossy);
    rt::IMember& room = rooms_.admit();
    viewer.take();
    viewer.unsent = kBehind;
    for (std::uint64_t seq = 1; seq <= 5; ++seq) {
        deliver(room, seq);
    }
    // Two of the five fit before its socket is behind again; the other three never reach it.
    viewer.growth = std::size_t{40} * 1024;
    viewer.unsent = 0;
    service_->drained(v);
    EXPECT_EQ(seqs(viewer.take()), (std::vector<std::uint64_t>{1, 2}));
    EXPECT_EQ(service_->counters().lossy_drops, 0U);
    service_->detach(v);
    EXPECT_EQ(service_->counters().lossy_drops, 3U);
}

TEST_F(ChatServiceTest, SeqsTheRoomDoesNotKeepAreCountedDroppedWhenAClientCatchesUpPastThem) {
    FakeClient viewer;
    const auto v = attach(viewer);
    join(v, std::nullopt, chat::Delivery::Lossy);
    rt::IMember& room = rooms_.admit();
    viewer.take();
    viewer.unsent = kBehind;
    deliver(room, 1);
    // 2 and 3 never reached this node (the room changed owners, or the message could not be
    // kept): catching up moves past them, and counts them, as it does past the room's newest
    // when that one was not kept.
    deliver(room, 4);
    viewer.unsent = 0;
    service_->drained(v);
    EXPECT_EQ(seqs(viewer.take()), (std::vector<std::uint64_t>{1, 4}));
    EXPECT_EQ(service_->counters().lossy_drops, 2U);
}

TEST_F(ChatServiceTest, EveryViewerOfALiveChatIsLossyWhateverItsJoinAsked) {
    const std::string live{kLiveRoom};
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
    const std::string live{kLiveRoom};
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
    const std::string live{kLiveRoom};
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

class FewKept : public ChatServiceTest {
protected:
    // A hundred messages across rooms, four ordinary ones per room.
    FewKept()
        : ChatServiceTest({.room_buffer_bytes = std::size_t{4} * 258, .buffer_messages = 100}) {}
};

// A busy live chat drops its own oldest with every message it keeps. What it dropped must not
// count against the rooms that keep theirs: a quiet group chat still resumes.
TEST_F(FewKept, AQuietRoomKeepsWhatItResumesFromWhileALiveChatRunsOnTheSameNode) {
    FakeClient member;
    FakeClient viewer;
    const auto m = attach(member);
    const auto v = attach(viewer, "bob");
    join(m);
    join(v, std::nullopt, chat::Delivery::Lossy, kLiveRoom);
    rt::IMember& quiet = rooms_.admit();
    rt::IMember& live = rooms_.admit();
    for (std::uint64_t seq = 1; seq <= 3; ++seq) {
        deliver(quiet, seq);
    }
    for (std::uint64_t seq = 1; seq <= 1'000; ++seq) {
        deliver(live, seq, "hi", "carol", kLiveRoom);
    }
    member.take();
    join(m, 0);
    EXPECT_EQ(seqs(member.take()), (std::vector<std::uint64_t>{1, 2, 3}));
    EXPECT_EQ(service_->buffered_bytes(), 7 * (2 + 256U));
}

// Entries of messages a room dropped itself are cleared before the order is a quarter over the
// bound: a live chat's every message leaves one, and none of them is ever read again.
TEST_F(FewKept, TheOrderOfKeptMessagesStaysWithinAQuarterOverTheBound) {
    FakeClient viewer;
    const auto v = attach(viewer);
    join(v, std::nullopt, chat::Delivery::Lossy, kLiveRoom);
    rt::IMember& live = rooms_.admit();
    std::size_t most = 0;
    for (std::uint64_t seq = 1; seq <= 1'000; ++seq) {
        deliver(live, seq, "hi", "carol", kLiveRoom);
        most = std::max(most, service_->kept_order_entries());
    }
    EXPECT_LE(most, 125U);
    // It did fill up to near the ceiling before clearing, not only after.
    EXPECT_GT(most, 100U);
}

class FewMessages : public ChatServiceTest {
protected:
    // Two ordinary messages per room, and four across rooms.
    FewMessages() : ChatServiceTest({.room_buffer_bytes = 600, .buffer_messages = 4}) {}
};

TEST_F(FewMessages, MessagesARoomAlreadyDroppedDoNotCountAgainstEveryRoomsLimit) {
    FakeClient alice;
    const auto a = attach(alice);
    join(a);
    join(a, std::nullopt, chat::Delivery::Durable, kOtherRoom);
    rt::IMember& quiet = rooms_.admit();
    rt::IMember& busy = rooms_.admit();
    deliver(quiet, 1);
    for (std::uint64_t seq = 1; seq <= 5; ++seq) {
        deliver(busy, seq, "hi", "bob", kOtherRoom);
    }
    // The busy room keeps its newest two, 4 and 5, and the quiet room its one: three messages
    // kept, under the four allowed across rooms, so none of the quiet room's had to go.
    EXPECT_EQ(service_->buffered_bytes(), 3 * (2 + 256U));
    alice.take();
    join(a, 0);
    EXPECT_EQ(seqs(alice.take()), std::vector<std::uint64_t>{1});
}

class TwoMessages : public ChatServiceTest {
protected:
    // Two ordinary messages per room, and two across rooms.
    TwoMessages() : ChatServiceTest({.room_buffer_bytes = 600, .buffer_messages = 2}) {}
};

TEST_F(TwoMessages, PastTheLimitAcrossRoomsTheOldestMessageAnywhereGoesFirst) {
    FakeClient alice;
    const auto a = attach(alice);
    join(a);
    join(a, std::nullopt, chat::Delivery::Durable, kOtherRoom);
    rt::IMember& first = rooms_.admit();
    rt::IMember& second = rooms_.admit();
    deliver(first, 1);
    deliver(second, 1, "hi", "bob", kOtherRoom);
    deliver(second, 2, "hi", "bob", kOtherRoom);
    // Each room is within its own limit; together they are one over, and the first room's
    // message is the oldest.
    EXPECT_EQ(service_->buffered_bytes(), 2 * (2 + 256U));
    alice.take();
    join(a, 0);
    EXPECT_TRUE(seqs(alice.take()).empty());
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

constexpr std::string_view kDevice = "01a0eb86-6cca-7dce-84cc-3bb47615f9aa";

std::vector<std::byte> answer(chat::CallOutcome outcome,
                              std::optional<core::ports::MediaTicket> ticket = std::nullopt) {
    return chat::encode_answer({.outcome = outcome, .ticket = std::move(ticket)});
}

TEST_F(ChatServiceTest, ACallIsAskedOfTheRoomsOwnerAndItsTicketHandedToTheClient) {
    FakeClient alice;
    const auto a = attach(alice);
    const chat::Call call{.room = room_id(), .device = *core::DeviceId::parse(kDevice)};
    // Not in the room yet: nothing is asked.
    service_->call(a, call);
    EXPECT_EQ(seen(alice.take().at(0)).reason, "not_joined");
    EXPECT_TRUE(rooms_.asks.empty());
    join(a);
    const rt::IMember& member = rooms_.admit();
    alice.take();

    service_->call(a, call);
    ASSERT_EQ(rooms_.asks.size(), 1U);
    EXPECT_EQ(rooms_.asks[0].room, room_id());
    EXPECT_EQ(rooms_.asks[0].member, &member);
    const auto request = chat::decode_request(rooms_.asks[0].request);
    ASSERT_TRUE(request);
    EXPECT_EQ(request->user.view(), "alice");
    EXPECT_EQ(request->device.to_string(), kDevice);
    rooms_.asks[0].done(answer(chat::CallOutcome::Ticket,
                               core::ports::MediaTicket{.endpoint = "wss://media.test",
                                                        .credential = "jwt",
                                                        .expires_at = core::WallTime{
                                                            std::chrono::seconds{1'790'000'060}}}));
    const auto got = alice.take();
    ASSERT_EQ(got.size(), 1U);
    const auto json = core::json::parse(got[0]);
    ASSERT_TRUE(json);
    EXPECT_EQ(json->find("type")->as_string(), "ticket");
    EXPECT_EQ(json->find("room")->as_string(), kRoom);
    EXPECT_EQ(json->find("url")->as_string(), "wss://media.test");
    EXPECT_EQ(json->find("token")->as_string(), "jwt");
    EXPECT_EQ(json->find("expires_at")->as_u64(), 1'790'000'060U);
}

TEST_F(ChatServiceTest, ACallTheOwnerRefusesOrCannotAnswerIsAnErrorWithARetryHintWhenOneHelps) {
    FakeClient alice;
    const auto a = attach(alice);
    join(a);
    rooms_.admit();
    alice.take();
    const chat::Call call{.room = room_id(), .device = *core::DeviceId::parse(kDevice)};
    const std::vector<std::pair<std::expected<std::vector<std::byte>, RouteError>, std::string>>
        cases{
            {answer(chat::CallOutcome::NotMember), "not_member"},
            {answer(chat::CallOutcome::NotCallable), "not_callable"},
            {answer(chat::CallOutcome::Unavailable), "unavailable"},
            {answer(chat::CallOutcome::Failed), "call_failed"},
            {answer(chat::CallOutcome::Disabled), "calls_disabled"},
            {answer(chat::CallOutcome::Busy), "busy"},
            {std::unexpected(RouteError::Unavailable), "unavailable"},
            {std::unexpected(RouteError::Busy), "busy"},
            {std::vector<std::byte>{std::byte{9}}, "unavailable"},
        };
    for (const auto& [result, reason] : cases) {
        service_->call(a, call);
        ASSERT_FALSE(rooms_.asks.empty());
        auto done = std::move(rooms_.asks.back().done);
        rooms_.asks.pop_back();
        done(result);
        const Seen s = seen(alice.take().at(0));
        EXPECT_EQ(s.type, "error");
        EXPECT_EQ(s.reason, reason);
        // Only what a retry may cure says when to retry.
        EXPECT_EQ(s.retry_after_ms.has_value(), reason == "unavailable") << reason;
    }
}

TEST_F(ChatServiceTest, ACallIsChargedAsAJoinAndADetachedClientHearsNoAnswer) {
    FakeClient alice;
    const auto a = attach(alice);
    join(a);
    rooms_.admit();
    alice.take();
    const chat::Call call{.room = room_id(), .device = *core::DeviceId::parse(kDevice)};
    // The join took one of the 64.
    for (int i = 0; i < 63; ++i) {
        service_->call(a, call);
    }
    EXPECT_EQ(rooms_.asks.size(), 63U);
    service_->call(a, call);
    EXPECT_EQ(rooms_.asks.size(), 63U);
    EXPECT_EQ(seen(alice.take().back()).reason, "busy");
    service_->detach(a);
    rooms_.asks[0].done(answer(chat::CallOutcome::NotMember));
    EXPECT_TRUE(alice.got.empty());
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
