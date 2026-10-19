#include "core/util/json.hpp"

#include "call_bell.hpp"
#include "presence_room.hpp"
#include "ring.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"

#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

class Socket final : public chat::IClient {
public:
    bool push(std::string_view text) noexcept override {
        if (closing) {
            return false;
        }
        got.emplace_back(text);
        return true;
    }
    [[nodiscard]] std::size_t unsent_bytes() const noexcept override { return 0; }
    void allocation_failed() noexcept override {}
    void disconnect() noexcept override {}

    std::vector<std::string> got;
    bool closing = false;
};

core::UserId user(std::string_view name) {
    return *core::UserId::parse(name);
}

class CallBellTest : public ::testing::Test {
protected:
    // What the ring sends `to` in their presence room.
    void hear(std::string_view to, chat::RingEvent event = chat::RingEvent::Ringing) {
        const auto bytes = chat::encode_notice({.event = event,
                                                .to = user(to),
                                                .room = room_,
                                                .call = call_,
                                                .from = user("alice"),
                                                .by = std::nullopt,
                                                .expires_at = clock_.wall_now()});
        bell_.on_notice(chat::presence_room(user(to)), bytes);
    }

    ulw::test::FakeClock clock_;
    ulw::test::FakeRandom random_;
    const core::RoomId room_ = core::RoomId::generate(clock_, random_);
    const chat::CallId call_ = chat::CallId::generate(clock_, random_);
    chat::CallBell bell_;
};

TEST_F(CallBellTest, ANoticeReachesEverySocketHereOfTheMemberItNamesAndNoOneElse) {
    Socket phone;
    Socket laptop;
    Socket alices;
    static_cast<void>(bell_.attach(phone, user("bob")));
    static_cast<void>(bell_.attach(laptop, user("bob")));
    static_cast<void>(bell_.attach(alices, user("alice")));
    EXPECT_EQ(bell_.users(), 2U);
    hear("bob");
    ASSERT_EQ(phone.got.size(), 1U);
    EXPECT_EQ(laptop.got, phone.got);
    EXPECT_TRUE(alices.got.empty());
    const auto json = core::json::parse(phone.got[0]);
    ASSERT_TRUE(json);
    EXPECT_EQ(json->find("type")->as_string(), "call_ringing");
    EXPECT_EQ(json->find("call")->as_string(), call_.to_string());
    EXPECT_EQ(json->find("from")->as_string(), "alice");
    EXPECT_EQ(bell_.counters().pushed, 2U);
}

TEST_F(CallBellTest, ADetachedSocketHearsNothingAndAUserWithNoneHereIsUnheard) {
    Socket phone;
    Socket laptop;
    const chat::BellId first = bell_.attach(phone, user("bob"));
    static_cast<void>(bell_.attach(laptop, user("bob")));
    bell_.detach(first);
    bell_.detach(first);
    hear("bob", chat::RingEvent::Answered);
    EXPECT_TRUE(phone.got.empty());
    EXPECT_EQ(laptop.got.size(), 1U);
    hear("carol");
    EXPECT_EQ(bell_.counters().unheard, 1U);
    // A socket that is closing takes nothing and is not counted.
    laptop.closing = true;
    hear("bob");
    EXPECT_EQ(bell_.counters().pushed, 1U);
}

TEST_F(CallBellTest, ANoticeOutsideItsMembersPresenceRoomOrThatDoesNotDecodeIsDropped) {
    Socket phone;
    static_cast<void>(bell_.attach(phone, user("bob")));
    const auto bytes = chat::encode_notice({.event = chat::RingEvent::Ringing,
                                            .to = user("bob"),
                                            .room = room_,
                                            .call = call_,
                                            .from = user("alice"),
                                            .by = std::nullopt,
                                            .expires_at = clock_.wall_now()});
    bell_.on_notice(chat::presence_room(user("alice")), bytes);
    bell_.on_notice(room_, bytes);
    const std::vector<std::byte> junk{std::byte{1}, std::byte{2}};
    bell_.on_notice(chat::presence_room(user("bob")), junk);
    EXPECT_TRUE(phone.got.empty());
    EXPECT_EQ(bell_.counters().malformed, 3U);
}

} // namespace

namespace {

// Answers rings_for when told to; nothing else is asked of it.
class FakeRings final : public core::ports::IRingStore {
public:
    void put_ring(core::ports::StoredRing /*ring*/,
                  core::ports::RingStoreCallback<void> done) override {
        done({});
    }
    void end_ring(const core::RoomId& /*room*/, const core::Uuid& /*call*/,
                  core::ports::RingStoreCallback<void> done) override {
        done({});
    }
    void
    ring_of(const core::RoomId& /*room*/,
            core::ports::RingStoreCallback<std::optional<core::ports::StoredRing>> done) override {
        done(std::optional<core::ports::StoredRing>{});
    }
    void
    rings_for(const core::UserId& user,
              core::ports::RingStoreCallback<std::vector<core::ports::StoredRing>> done) override {
        asked.emplace_back(user.view());
        waiting.push_back(std::move(done));
    }
    std::vector<std::string> asked;
    std::vector<core::ports::RingStoreCallback<std::vector<core::ports::StoredRing>>> waiting;
};

// ADR-0129: a socket hears its user's rings as it connects, from the store.
TEST_F(CallBellTest, ASocketHearsItsUsersRingsAsItConnects) {
    FakeRings rings;
    bell_.replay_from(&rings);
    Socket phone;
    static_cast<void>(bell_.attach(phone, user("bob")));
    ASSERT_EQ(rings.asked, std::vector<std::string>{"bob"});
    std::move(rings.waiting.back())(std::vector<core::ports::StoredRing>{
        {.room = room_,
         .call = call_.uuid(),
         .caller = user("alice"),
         .group = false,
         .callees = {user("bob")},
         .started_at = clock_.wall_now(),
         .expires_at = clock_.wall_now() + core::Millis{30'000}}});
    ASSERT_EQ(phone.got.size(), 1U);
    const auto json = core::json::parse(phone.got[0]);
    ASSERT_TRUE(json);
    EXPECT_EQ(json->find("type")->as_string(), "call_ringing");
    EXPECT_EQ(json->find("call")->as_string(), call_.to_string());
    EXPECT_EQ(json->find("from")->as_string(), "alice");
    EXPECT_EQ(bell_.counters().replayed, 1U);
    // A socket gone before the answer hears nothing, and a read that failed is counted.
    Socket laptop;
    const auto gone = bell_.attach(laptop, user("bob"));
    bell_.detach(gone);
    std::move(rings.waiting.back())(std::vector<core::ports::StoredRing>{});
    EXPECT_TRUE(laptop.got.empty());
    Socket tablet;
    static_cast<void>(bell_.attach(tablet, user("bob")));
    std::move(rings.waiting.back())(std::unexpected(core::ports::RingStoreError::Unavailable));
    EXPECT_EQ(bell_.counters().replay_failures, 1U);
    bell_.replay_from(nullptr);
}

} // namespace
