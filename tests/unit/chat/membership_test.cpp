// The chat service's member-list commands (ADR-0096) on the in-memory store, which keeps the same
// laws as Postgres (conformance/message_store_conformance_test.cpp): what each command answers
// the client that asked, what it costs, and who on this node hears of each change.
#include "core/util/json.hpp"
#include "infra/messages/memory_message_store.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"

#include "chat_service.hpp"
#include "named_rooms.hpp"
#include "support/fake_clock.hpp"
#include "support/reactor_harness.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using core::Millis;

core::UserId user(std::string_view id) {
    return *core::UserId::parse(id);
}

// The room plane: joins answered at once with seq 0. Nothing here sends.
class Rooms final : public chat::IRooms {
public:
    void join(const core::RoomId& /*room*/, rt::IMember& /*member*/,
              rt::JoinCallback done) override {
        done(std::uint64_t{0});
    }
    void leave(const core::RoomId& /*room*/, rt::IMember& /*member*/) noexcept override {}
    void send(const core::RoomId& /*room*/, rt::IMember& /*from*/, const core::UserId& /*sender*/,
              const rt::MessageKey& /*key*/, std::vector<std::byte> /*body*/,
              rt::SendCallback done) override {
        done(std::unexpected(rt::RouteError::Unavailable));
    }
    void ask_owner(const core::RoomId& /*room*/, rt::IMember& /*from*/,
                   std::span<const std::byte> /*request*/, rt::OwnerAnswer done) override {
        done(std::unexpected(rt::RouteError::Unavailable));
    }
};

class Client final : public chat::IClient {
public:
    bool push(std::string_view text) noexcept override {
        if (closing) {
            return false;
        }
        got.emplace_back(text);
        return true;
    }
    [[nodiscard]] std::size_t unsent_bytes() const noexcept override { return 0; }
    void allocation_failed() noexcept override { ++failures; }

    std::vector<std::string> take() { return std::exchange(got, {}); }

    std::vector<std::string> got;
    bool closing = false;
    int failures = 0;
};

// A frame's fields, as text.
std::string field(const std::string& text, std::string_view key) {
    const auto json = core::json::parse(text);
    EXPECT_TRUE(json) << text;
    const core::json::Value* v = json ? json->find(key) : nullptr;
    if (v == nullptr) {
        return "";
    }
    if (const auto s = v->as_string()) {
        return std::string(*s);
    }
    if (const auto n = v->as_u64()) {
        return std::to_string(*n);
    }
    if (const auto b = v->as_bool()) {
        return *b ? "true" : "false";
    }
    if (const auto* a = v->as_array()) {
        return std::to_string(a->size());
    }
    return "?";
}

class MembershipTest : public ::testing::Test {
protected:
    explicit MembershipTest(chat::ServiceLimits limits = {}) : limits_(limits) {}

    void SetUp() override {
        auto reactor = net::make_reactor(net::ReactorKind::Epoll, system_clock_, 64);
        ASSERT_TRUE(reactor);
        reactor_ = std::move(*reactor);
        store_ = std::make_unique<infra::messages::MemoryMessageStore>(*reactor_);
        service_ = std::make_unique<chat::ChatService>(rooms_, *store_, clock_, limits_);
    }

    void TearDown() override {
        // The store first, as the service's contract asks.
        store_.reset();
        service_.reset();
    }

    chat::ClientId attach(Client& client, std::string_view name) {
        return service_->attach(client, user(name));
    }

    // Pumps until `client` has an answer, a frame other than a member list's change, and returns
    // it; the changes told before it are dropped, those after it kept.
    std::string next(Client& client) {
        const auto answer = [&] {
            return std::ranges::find_if(client.got, [](const std::string& frame) {
                return field(frame, "type") != "member";
            });
        };
        EXPECT_TRUE(ulw::test::pump_until(*reactor_, [&] { return answer() != client.got.end(); }))
            << "no answer";
        const auto at = answer();
        if (at == client.got.end()) {
            return "{}";
        }
        std::string first = *at;
        client.got.erase(client.got.begin(), at + 1);
        return first;
    }

    // Runs the store's answers and whatever they tell, and returns what reached `client`.
    std::vector<std::string> settle(Client& client) {
        ulw::test::pump_pending(*reactor_);
        return client.take();
    }

    // Opens bob's direct chat with alice, as alice, and returns its room.
    core::RoomId direct(chat::ClientId alice, Client& client) {
        service_->open_direct(alice, {.user = user("bob")});
        const std::string answer = next(client);
        EXPECT_EQ(field(answer, "type"), "direct") << answer;
        return *core::RoomId::parse(field(answer, "room"));
    }

    // A group of alice (admin), bob and carol, made by alice.
    core::RoomId group(chat::ClientId alice, Client& client, std::string_view id = "g1") {
        service_->create_group(
            alice, {.id = *rt::MessageKey::parse(id), .users = {user("bob"), user("carol")}});
        const std::string answer = next(client);
        EXPECT_EQ(field(answer, "type"), "group") << answer;
        settle(client);
        return *core::RoomId::parse(field(answer, "room"));
    }

    void join(chat::ClientId id, Client& client, const core::RoomId& room) {
        service_->join(
            id, {.room = room,
                 .after = std::nullopt,
                 .delivery = chat::Delivery::Durable,
                 .kind = core::ports::named_kind(room).value_or(core::ports::RoomKind::GroupChat)});
        const std::string answer = next(client);
        ASSERT_EQ(field(answer, "type"), "joined") << answer;
    }

    chat::ServiceLimits limits_;
    os::SystemClock system_clock_;
    ulw::test::FakeClock clock_;
    Rooms rooms_;
    std::unique_ptr<net::IReactor> reactor_;
    std::unique_ptr<infra::messages::MemoryMessageStore> store_;
    std::unique_ptr<chat::ChatService> service_;
};

TEST_F(MembershipTest, ADirectChatIsTheSameRoomWhicheverOfThePairOpensItAndTheirsAlone) {
    Client alice;
    Client bob;
    const auto a = attach(alice, "alice");
    const auto b = attach(bob, "bob");
    const core::RoomId room = direct(a, alice);
    EXPECT_EQ(room, chat::direct_room(user("alice"), user("bob")));
    // Each of the two hears that they were listed, wherever they are on this node.
    std::vector<std::string> heard = settle(alice);
    ASSERT_EQ(heard.size(), 1U);
    EXPECT_EQ(field(heard[0], "type"), "member");
    EXPECT_EQ(field(heard[0], "user"), "alice");
    EXPECT_EQ(field(heard[0], "change"), "added");
    heard = settle(bob);
    ASSERT_EQ(heard.size(), 1U);
    EXPECT_EQ(field(heard[0], "user"), "bob");

    service_->open_direct(b, {.user = user("alice")});
    const std::string answer = next(bob);
    EXPECT_EQ(answer,
              std::format(R"({{"type":"direct","room":"{}","user":"alice"}})", room.to_string()));
    EXPECT_TRUE(settle(bob).empty()) << "a repeat listed someone";
    join(a, alice, room);
    join(b, bob, room);
    EXPECT_EQ(service_->counters().directs_opened, 1U);

    Client carol;
    const auto c = attach(carol, "carol");
    service_->join(c, {.room = room,
                       .after = std::nullopt,
                       .delivery = chat::Delivery::Durable,
                       .kind = core::ports::RoomKind::DirectChat});
    EXPECT_EQ(field(next(carol), "reason"), "not_member");
}

TEST_F(MembershipTest, NobodyOpensADirectChatWithThemselves) {
    Client alice;
    const auto a = attach(alice, "alice");
    service_->open_direct(a, {.user = user("alice")});
    EXPECT_EQ(alice.take(),
              std::vector<std::string>{R"({"type":"error","reason":"self","user":"alice"})"});
}

TEST_F(MembershipTest, AGroupIsCreatedOnceForItsRequestAndItsCreatorIsItsAdmin) {
    Client alice;
    Client bob;
    const auto a = attach(alice, "alice");
    attach(bob, "bob");
    // The creator named among the users, and a user twice, list each once.
    service_->create_group(a, {.id = *rt::MessageKey::parse("g1"),
                               .users = {user("bob"), user("alice"), user("bob")}});
    const std::string answer = next(alice);
    const core::RoomId room = chat::group_room(user("alice"), *rt::MessageKey::parse("g1"));
    EXPECT_EQ(answer, std::format(R"({{"type":"group","room":"{}","id":"g1"}})", room.to_string()));
    EXPECT_EQ(settle(bob).size(), 1U);
    // The same request again, after a lost answer, names the same room and lists nobody more.
    service_->create_group(a, {.id = *rt::MessageKey::parse("g1"), .users = {user("carol")}});
    EXPECT_EQ(next(alice), answer);
    EXPECT_TRUE(settle(alice).empty());
    EXPECT_EQ(service_->counters().groups_created, 1U);

    service_->list_members(a, {.room = room, .after = std::nullopt, .limit = 1});
    EXPECT_EQ(
        next(alice),
        std::format(
            R"({{"type":"members","room":"{}","members":[{{"user":"alice","role":"admin"}}],"more":true}})",
            room.to_string()));
    service_->list_members(a, {.room = room, .after = user("alice"), .limit = 10});
    EXPECT_EQ(
        next(alice),
        std::format(
            R"({{"type":"members","room":"{}","members":[{{"user":"bob","role":"member"}}],"more":false}})",
            room.to_string()));
}

TEST_F(MembershipTest, AnAdminAddsAndRemovesAndEveryoneInTheRoomHearsIt) {
    Client alice;
    Client bob;
    Client dave;
    const auto a = attach(alice, "alice");
    const auto b = attach(bob, "bob");
    const auto d = attach(dave, "dave");
    const core::RoomId room = group(a, alice);
    settle(bob);
    join(b, bob, room);

    service_->add_members(b, {.room = room, .users = {user("dave")}});
    EXPECT_EQ(field(next(bob), "reason"), "not_admin");
    service_->add_members(d, {.room = room, .users = {user("dave")}});
    EXPECT_EQ(field(next(dave), "reason"), "not_member");

    // Naming the asker and someone listed already adds only the new one.
    service_->add_members(a, {.room = room, .users = {user("dave"), user("alice"), user("bob")}});
    EXPECT_EQ(next(alice),
              std::format(R"({{"type":"added","room":"{}","users":["dave"]}})", room.to_string()));
    // Dave hears he was listed; bob, in the room, hears it too; alice is not in it here.
    EXPECT_EQ(settle(dave), std::vector<std::string>{std::format(
                                R"({{"type":"member","room":"{}","user":"dave","change":"added"}})",
                                room.to_string())});
    EXPECT_EQ(settle(bob).size(), 1U);
    EXPECT_TRUE(settle(alice).empty());
    EXPECT_EQ(service_->counters().members_added, 1U);

    service_->remove_member(a, {.room = room, .user = user("bob")});
    EXPECT_EQ(next(alice),
              std::format(R"({{"type":"removed","room":"{}","user":"bob"}})", room.to_string()));
    // Bob is out of the room at once, and told so.
    const std::vector<std::string> told = settle(bob);
    ASSERT_EQ(told.size(), 2U);
    EXPECT_EQ(field(told[0], "reason"), "not_member");
    EXPECT_EQ(field(told[1], "change"), "removed");
    EXPECT_EQ(service_->counters().members_removed, 1U);
    EXPECT_EQ(service_->counters().membership_not_admin, 1U);
    EXPECT_EQ(service_->counters().membership_not_member, 1U);
}

TEST_F(MembershipTest, LeavingIsForAGroupAndItsLastAdminHandsOn) {
    Client alice;
    Client bob;
    const auto a = attach(alice, "alice");
    const auto b = attach(bob, "bob");
    const core::RoomId room = group(a, alice);
    service_->leave_room(a, {.room = room});
    // Bob, first of those left, is its admin now, and everyone in the room hears so.
    EXPECT_EQ(next(alice),
              std::format(R"({{"type":"left","room":"{}","promoted":"bob"}})", room.to_string()));
    const auto told = settle(bob);
    EXPECT_NE(
        std::ranges::find(
            told, std::format(R"({{"type":"member","room":"{}","user":"bob","change":"promoted"}})",
                              room.to_string())),
        told.end());
    service_->list_members(b, {.room = room, .after = std::nullopt, .limit = 10});
    EXPECT_EQ(field(next(bob), "members"), "2");
    // As admin he can remove carol.
    service_->remove_member(b, {.room = room, .user = user("carol")});
    EXPECT_EQ(field(next(bob), "type"), "removed");
    // Removing oneself is leaving.
    service_->remove_member(b, {.room = room, .user = user("bob")});
    EXPECT_EQ(field(next(bob), "type"), "removed");
    EXPECT_EQ(service_->counters().members_left, 2U);

    const core::RoomId pair = direct(a, alice);
    service_->leave_room(a, {.room = pair});
    EXPECT_EQ(field(next(alice), "reason"), "not_group");
    service_->add_members(a, {.room = pair, .users = {user("carol")}});
    EXPECT_EQ(field(next(alice), "reason"), "not_group");
    EXPECT_EQ(service_->counters().membership_not_group, 2U);
}

TEST_F(MembershipTest, AUsersRoomsArePagedWithWhatTheyAre) {
    Client alice;
    const auto a = attach(alice, "alice");
    const core::RoomId pair = direct(a, alice);
    const core::RoomId mine = group(a, alice);
    settle(alice);
    service_->list_rooms(a, {.after = std::nullopt, .limit = 1});
    const std::string first = next(alice);
    EXPECT_EQ(field(first, "more"), "true");
    EXPECT_EQ(field(first, "rooms"), "1");
    // Direct chats (03...) sort before group chats (04...).
    EXPECT_EQ(
        first,
        std::format(
            R"({{"type":"rooms","rooms":[{{"room":"{}","kind":"direct","role":"member","peer":"bob"}}],"more":true}})",
            pair.to_string()));
    service_->list_rooms(a, {.after = pair, .limit = 1});
    EXPECT_EQ(
        next(alice),
        std::format(
            R"({{"type":"rooms","rooms":[{{"room":"{}","kind":"group","role":"admin"}}],"more":false}})",
            mine.to_string()));
    Client dave;
    const auto d = attach(dave, "dave");
    service_->list_rooms(d, {.after = std::nullopt, .limit = 50});
    EXPECT_EQ(next(dave), R"({"type":"rooms","rooms":[],"more":false})");
    service_->list_members(d, {.room = mine, .after = std::nullopt, .limit = 50});
    EXPECT_EQ(next(dave), std::format(R"({{"type":"error","reason":"not_member","room":"{}"}})",
                                      mine.to_string()));
}

class TightMembership : public MembershipTest {
protected:
    TightMembership()
        : MembershipTest(
              chat::ServiceLimits{.membership_burst = 2, .membership_interval = Millis{3'000}}) {}
};

// Changes are paced per user; reads are charged as joins, not to that allowance.
TEST_F(TightMembership, ChangesPastTheAllowanceAreRefusedWithWhenToAskAgain) {
    Client alice;
    const auto a = attach(alice, "alice");
    direct(a, alice);
    settle(alice);
    service_->open_direct(a, {.user = user("carol")});
    EXPECT_EQ(field(next(alice), "type"), "direct");
    settle(alice);
    service_->create_group(a, {.id = *rt::MessageKey::parse("g"), .users = {}});
    EXPECT_EQ(alice.take(),
              std::vector<std::string>{
                  R"({"type":"error","reason":"rate_limited","id":"g","retry_after_ms":3000})"});
    const core::RoomId room = *core::RoomId::parse("01a0eb86-6cca-7dce-84cc-3bb47615f9fd");
    for (const auto& ask : std::vector<std::function<void()>>{
             [&] { service_->add_members(a, {.room = room, .users = {user("d")}}); },
             [&] { service_->remove_member(a, {.room = room, .user = user("d")}); },
             [&] { service_->leave_room(a, {.room = room}); },
             [&] { service_->open_direct(a, {.user = user("d")}); }}) {
        ask();
        const auto got = alice.take();
        ASSERT_EQ(got.size(), 1U);
        EXPECT_EQ(field(got[0], "reason"), "rate_limited");
        EXPECT_EQ(field(got[0], "retry_after_ms"), "3000");
    }
    EXPECT_EQ(service_->counters().membership_rate_limited, 5U);
    service_->list_rooms(a, {.after = std::nullopt, .limit = 10});
    EXPECT_EQ(field(next(alice), "rooms"), "2");
    clock_.advance(Millis{3'000});
    service_->create_group(a, {.id = *rt::MessageKey::parse("g"), .users = {}});
    EXPECT_EQ(field(next(alice), "type"), "group");
}

// A named room recorded as another kind can only be a store that was written past its
// constraints (migration 0014): the client is answered unavailable, as for a store it cannot read.
TEST_F(MembershipTest, ADirectChatRecordedAsAGroupIsAnsweredUnavailable) {
    Client alice;
    const auto a = attach(alice, "alice");
    const core::RoomId room = chat::direct_room(user("alice"), user("bob"));
    store_->add_member(room, user("carol"), [](core::ports::MessageResult<void>) noexcept {});
    settle(alice);
    service_->open_direct(a, {.user = user("bob")});
    EXPECT_EQ(next(alice), R"({"type":"error","reason":"unavailable","user":"bob"})");
    EXPECT_EQ(service_->counters().membership_unavailable, 1U);
}

TEST_F(MembershipTest, AStreamsLiveChatIsNoListAnyoneHearsOf) {
    Client alice;
    const auto a = attach(alice, "alice");
    const core::RoomId live = *core::RoomId::parse("011b9ed0-d6b6-88e6-ac34-32d7070ba83b");
    service_->on_member_added(live, user("alice"));
    service_->on_member_removed(live, user("alice"));
    EXPECT_TRUE(alice.take().empty());
    (void)a;
}

TEST_F(MembershipTest, AClientGoneBeforeItsAnswerIsAnsweredNothing) {
    Client alice;
    const auto a = attach(alice, "alice");
    service_->open_direct(a, {.user = user("bob")});
    service_->list_rooms(a, {.after = std::nullopt, .limit = 10});
    service_->detach(a);
    EXPECT_TRUE(settle(alice).empty());
}

TEST_F(MembershipTest, AClosingConnectionIsNotCountedAsTold) {
    Client alice;
    Client bob;
    const auto a = attach(alice, "alice");
    attach(bob, "bob");
    bob.closing = true;
    direct(a, alice);
    settle(alice);
    // Alice's own addition was told; bob's socket was closing and took nothing.
    EXPECT_EQ(service_->counters().member_events, 1U);
}

// A page asks the store for one entry more than it shows: at the largest page, a 101st entry
// says there is more.
TEST_F(MembershipTest, AFullPageOfTheLargestSizeSaysWhetherMoreFollow) {
    Client alice;
    const auto a = attach(alice, "alice");
    const core::RoomId crowded = *core::RoomId::parse("01a0eb86-6cca-7dce-84cc-3bb47615f9fd");
    std::string text = crowded.to_string();
    for (int i = 0; i < 101; ++i) {
        text.replace(24, 12, std::format("{:012x}", i));
        store_->add_member(*core::RoomId::parse(text), user("alice"),
                           [](core::ports::MessageResult<void>) noexcept {});
        store_->add_member(crowded, user(std::format("u{:03}", i)),
                           [](core::ports::MessageResult<void>) noexcept {});
    }
    store_->add_member(crowded, user("alice"), [](core::ports::MessageResult<void>) noexcept {});
    settle(alice);
    service_->list_rooms(a, {.after = std::nullopt, .limit = chat::kMaxListLimit});
    std::string page = next(alice);
    EXPECT_EQ(field(page, "rooms"), "100");
    EXPECT_EQ(field(page, "more"), "true");
    service_->list_members(a, {.room = crowded, .after = std::nullopt, .limit = 100});
    page = next(alice);
    EXPECT_EQ(field(page, "members"), "100");
    EXPECT_EQ(field(page, "more"), "true");
    service_->list_members(a, {.room = crowded, .after = user("u099"), .limit = 100});
    page = next(alice);
    EXPECT_EQ(field(page, "members"), "1");
    EXPECT_EQ(field(page, "more"), "false");
}

// What one account may make the database hold, whatever node it asks.
TEST_F(MembershipTest, AUserInTheMostRoomsCreatesNoMore) {
    Client alice;
    const auto a = attach(alice, "alice");
    std::string text = "01a0eb86-6cca-7dce-84cc-000000000000";
    for (std::size_t i = 0; i < core::ports::kMaxRoomsPerUser; ++i) {
        text.replace(24, 12, std::format("{:012x}", i));
        store_->add_member(*core::RoomId::parse(text), user("alice"),
                           [](core::ports::MessageResult<void>) noexcept {});
    }
    settle(alice);
    service_->open_direct(a, {.user = user("bob")});
    EXPECT_EQ(next(alice), R"({"type":"error","reason":"room_limit","user":"bob"})");
    service_->create_group(a, {.id = *rt::MessageKey::parse("g"), .users = {}});
    EXPECT_EQ(field(next(alice), "reason"), "room_limit");
    EXPECT_EQ(service_->counters().membership_room_limit, 2U);
    // Others may still add her.
    Client bob;
    const auto b = attach(bob, "bob");
    service_->open_direct(b, {.user = user("alice")});
    EXPECT_EQ(field(next(bob), "type"), "direct");
}

// A group everyone left keeps its history: creating it again under its id would hand that to
// whoever the create lists.
TEST_F(MembershipTest, AGroupEveryoneLeftIsNotCreatedAgainOverItsHistory) {
    Client alice;
    const auto a = attach(alice, "alice");
    service_->create_group(a, {.id = *rt::MessageKey::parse("g"), .users = {}});
    const core::RoomId room = *core::RoomId::parse(field(next(alice), "room"));
    store_->append(room, 1, user("alice"), "m1", {std::byte{1}}, core::WallTime{},
                   [](core::ports::MessageResult<std::uint64_t>) noexcept {});
    service_->leave_room(a, {.room = room});
    EXPECT_EQ(field(next(alice), "type"), "left");
    service_->create_group(a, {.id = *rt::MessageKey::parse("g"), .users = {user("mallory")}});
    EXPECT_EQ(next(alice), std::format(R"({{"type":"error","reason":"gone","id":"g"}})"));
    EXPECT_EQ(service_->counters().membership_gone, 1U);
}

TEST_F(MembershipTest, AGroupsRoomRecordedAsADirectChatIsAnsweredUnavailable) {
    Client alice;
    const auto a = attach(alice, "alice");
    const core::RoomId room = chat::group_room(user("alice"), *rt::MessageKey::parse("g"));
    store_->admits(room, user("carol"), core::ports::RoomKind::DirectChat,
                   [](core::ports::MessageResult<core::ports::Admission>) noexcept {});
    settle(alice);
    service_->create_group(a, {.id = *rt::MessageKey::parse("g"), .users = {}});
    EXPECT_EQ(next(alice), R"({"type":"error","reason":"unavailable","id":"g"})");
}

// Who becomes an admin, or stops being one, is told as a change; a stream's chat has no list.
TEST_F(MembershipTest, ARoleChangeIsToldAsPromotedOrDemoted) {
    Client alice;
    const auto a = attach(alice, "alice");
    const core::RoomId room = group(a, alice);
    service_->on_member_role(room, user("alice"), core::ports::MemberRole::Member);
    service_->on_member_role(room, user("alice"), core::ports::MemberRole::Admin);
    const auto got = alice.take();
    ASSERT_EQ(got.size(), 2U);
    EXPECT_EQ(field(got[0], "change"), "demoted");
    EXPECT_EQ(field(got[1], "change"), "promoted");
    service_->on_member_role(*core::RoomId::parse("011b9ed0-d6b6-88e6-ac34-32d7070ba83b"),
                             user("alice"), core::ports::MemberRole::Admin);
    EXPECT_TRUE(alice.take().empty());
}

} // namespace
