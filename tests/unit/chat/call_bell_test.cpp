#include "core/util/json.hpp"

#include "call_bell.hpp"
#include "presence_room.hpp"
#include "ring.hpp"
#include "room_notices.hpp"
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

namespace {

// ADR-0137: a room's activity, told to a member's presence room, reaches every socket here of
// that member as an activity frame, with no body.
TEST_F(CallBellTest, ARoomsActivityReachesEverySocketOfTheMemberItNames) {
    Socket phone;
    Socket laptop;
    Socket alices;
    static_cast<void>(bell_.attach(phone, user("bob")));
    static_cast<void>(bell_.attach(laptop, user("bob")));
    static_cast<void>(bell_.attach(alices, user("alice")));
    const chat::ActivityNotice notice{.to = user("bob"),
                                      .room = room_,
                                      .seq = 12,
                                      .sender = user("alice"),
                                      .at = clock_.wall_now() + core::Millis{345},
                                      .quiet = false};
    const auto bytes = chat::encode_activity(notice);
    EXPECT_TRUE(chat::is_activity(bytes));
    EXPECT_FALSE(chat::decode_notice(bytes));
    bell_.on_notice(chat::presence_room(user("bob")), bytes);
    ASSERT_EQ(phone.got.size(), 1U);
    EXPECT_EQ(laptop.got, phone.got);
    EXPECT_TRUE(alices.got.empty());
    EXPECT_EQ(phone.got[0], R"({"type":"activity","room":")" + room_.to_string() +
                                R"(","seq":12,"sender":"alice","at":1767225600345,)"
                                R"("quiet":false})");
    EXPECT_EQ(bell_.counters().activity, 2U);

    // Outside the member's presence room, or cut short, or with a byte too many: dropped.
    bell_.on_notice(chat::presence_room(user("alice")), bytes);
    bell_.on_notice(room_, bytes);
    auto cut = bytes;
    cut.pop_back();
    bell_.on_notice(chat::presence_room(user("bob")), cut);
    auto longer = bytes;
    longer.push_back(std::byte{0});
    bell_.on_notice(chat::presence_room(user("bob")), longer);
    auto bad_quiet = bytes;
    bad_quiet.back() = std::byte{2};
    bell_.on_notice(chat::presence_room(user("bob")), bad_quiet);
    EXPECT_EQ(bell_.counters().malformed, 5U);
    EXPECT_EQ(phone.got.size(), 1U);
    EXPECT_FALSE(chat::decode_activity(chat::encode_notice({.event = chat::RingEvent::Ringing,
                                                            .to = user("bob"),
                                                            .room = room_,
                                                            .call = call_,
                                                            .from = user("alice"),
                                                            .by = std::nullopt,
                                                            .expires_at = clock_.wall_now()})));
    EXPECT_FALSE(chat::is_activity({}));

    // A member with no socket here hears nothing.
    const auto carols = chat::encode_activity({.to = user("carol"),
                                               .room = room_,
                                               .seq = 12,
                                               .sender = user("alice"),
                                               .at = clock_.wall_now(),
                                               .quiet = true});
    bell_.on_notice(chat::presence_room(user("carol")), carols);
    EXPECT_EQ(bell_.counters().unheard, 1U);
    const auto decoded = chat::decode_activity(carols);
    ASSERT_TRUE(decoded);
    EXPECT_TRUE(decoded->quiet);
    EXPECT_EQ(decoded->to, user("carol"));
    EXPECT_EQ(decoded->at, clock_.wall_now());
}

} // namespace

namespace {

// ADR-0138: a notice of a room that is not a presence room goes to whoever hears the rooms, as it
// came; typing and read marks read back as they were written, and nothing else does.
TEST_F(CallBellTest, ARoomsOwnNoticesGoToTheRoomsListenerAndReadBackWhole) {
    struct Rooms final : rt::INoticeListener {
        void on_notice(const core::RoomId& room,
                       std::span<const std::byte> body) noexcept override {
            heard.emplace_back(room, std::vector<std::byte>{body.begin(), body.end()});
        }
        std::vector<std::pair<core::RoomId, std::vector<std::byte>>> heard;
    } rooms;
    bell_.hear_rooms(&rooms);
    const auto typing = chat::encode_typing({.room = room_, .user = user("alice")});
    const auto read = chat::encode_read({.room = room_, .user = user("bob"), .seq = 9});
    bell_.on_notice(room_, typing);
    bell_.on_notice(room_, read);
    ASSERT_EQ(rooms.heard.size(), 2U);
    EXPECT_EQ(rooms.heard[0].first, room_);
    EXPECT_EQ(rooms.heard[1].second, read);
    // A presence room's notices are still the bell's.
    bell_.on_notice(chat::presence_room(user("bob")), typing);
    EXPECT_EQ(rooms.heard.size(), 2U);
    EXPECT_EQ(bell_.counters().malformed, 1U);
    bell_.hear_rooms(nullptr);

    const auto t = chat::decode_room_notice(typing);
    ASSERT_TRUE(t);
    EXPECT_EQ(std::get<chat::TypingNotice>(*t).user, user("alice"));
    EXPECT_EQ(std::get<chat::TypingNotice>(*t).room, room_);
    const auto r = chat::decode_room_notice(read);
    ASSERT_TRUE(r);
    EXPECT_EQ(std::get<chat::ReadNotice>(*r).seq, 9U);
    EXPECT_EQ(std::get<chat::ReadNotice>(*r).user, user("bob"));
    for (auto bad : {typing, read}) {
        auto longer = bad;
        longer.push_back(std::byte{0});
        EXPECT_FALSE(chat::decode_room_notice(longer));
        bad.pop_back();
        EXPECT_FALSE(chat::decode_room_notice(bad));
    }
    auto no_user = typing;
    no_user.resize(1 + core::Uuid::kTextLength);
    EXPECT_FALSE(chat::decode_room_notice(no_user));
    EXPECT_FALSE(chat::decode_room_notice({}));
    EXPECT_FALSE(chat::decode_room_notice(chat::encode_activity({.to = user("bob"),
                                                                 .room = room_,
                                                                 .seq = 1,
                                                                 .sender = user("alice"),
                                                                 .at = clock_.wall_now(),
                                                                 .quiet = false})));
}

} // namespace
