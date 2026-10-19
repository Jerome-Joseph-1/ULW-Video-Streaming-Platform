#include "core/util/json.hpp"
#include "infra/notify/memory_connectivity.hpp"
#include "net/reactor_factory.hpp"

#include "live_chat.hpp"
#include "notify.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using core::ports::NotifyCallback;
using core::ports::SocketDevice;

constexpr std::string_view kRoom = "01a0eb86-6cca-7dce-84cc-3bb47615f9fd";
constexpr std::string_view kPhone = "01a0eb86-6cca-7dce-84cc-3bb47615f001";
constexpr std::string_view kLaptop = "01a0eb86-6cca-7dce-84cc-3bb47615f002";
constexpr std::uint64_t kNode = 0x0123'4567'89ab'cdefULL;

core::UserId user(std::string_view name) {
    return *core::UserId::parse(name);
}

core::DeviceId device(std::string_view id) {
    return *core::DeviceId::parse(id);
}

core::RoomId room_id() {
    return *core::RoomId::parse(kRoom);
}

class CollectingSink final : public chat::IEventSink {
public:
    void offer(std::string event) noexcept override { events.push_back(std::move(event)); }

    // Each event parsed; a test fails on one that is not JSON.
    std::vector<core::json::Value> take() {
        std::vector<core::json::Value> out;
        for (const std::string& e : events) {
            auto v = core::json::parse(e);
            EXPECT_TRUE(v) << e;
            if (v) {
                out.push_back(std::move(*v));
            }
        }
        events.clear();
        return out;
    }

    std::vector<std::string> events;
};

// The memory registry, but connected() answers only when told to.
class StallingStore final : public core::ports::IConnectivityStore {
public:
    explicit StallingStore(core::ports::IConnectivityStore& inner) noexcept : inner_(inner) {}

    void renew(std::uint64_t node, NotifyCallback<bool> done) override {
        inner_.renew(node, std::move(done));
    }
    void sync(std::uint64_t node, std::vector<SocketDevice> added,
              std::vector<SocketDevice> removed, NotifyCallback<void> done) override {
        inner_.sync(node, std::move(added), std::move(removed), std::move(done));
    }
    void forget(std::uint64_t node, NotifyCallback<void> done) override {
        inner_.forget(node, std::move(done));
    }
    void connected(std::vector<core::UserId> /*users*/,
                   NotifyCallback<std::vector<SocketDevice>> done) override {
        stalled.push_back(std::move(done));
    }
    void message_audience(const core::RoomId& room, const core::UserId& sender,
                          NotifyCallback<core::ports::MessageAudience> done) override {
        inner_.message_audience(room, sender, std::move(done));
    }
    void message_targets(const core::RoomId& room, const core::UserId& sender,
                         NotifyCallback<std::vector<core::UserId>> done) override {
        inner_.message_targets(room, sender, std::move(done));
    }

    std::vector<NotifyCallback<std::vector<SocketDevice>>> stalled;

private:
    core::ports::IConnectivityStore& inner_;
};

std::string_view text(const core::json::Value& v, std::string_view key) {
    const core::json::Value* f = v.find(key);
    return f != nullptr ? f->as_string().value_or("") : "";
}

std::vector<std::string> strings(const core::json::Value* v) {
    std::vector<std::string> out;
    if (v == nullptr || v->as_array() == nullptr) {
        return out;
    }
    for (const core::json::Value& item : *v->as_array()) {
        out.emplace_back(item.as_string().value_or(""));
    }
    return out;
}

class NotifierTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto r = net::make_reactor(net::ReactorKind::Epoll, clock_, 256);
        ASSERT_TRUE(r);
        reactor_ = std::move(*r);
        registry_ = std::make_unique<infra::notify::MemoryConnectivity>(
            *reactor_, clock_, core::ports::NotifyTiming{});
        stalling_ = std::make_unique<StallingStore>(*registry_);
    }
    void TearDown() override {
        notifier_.reset();
        stalling_.reset();
        registry_.reset();
        reactor_.reset();
    }

    void make(bool stall = false, chat::NotifyLimits limits = {}) {
        core::ports::IConnectivityStore& store =
            stall ? static_cast<core::ports::IConnectivityStore&>(*stalling_) : *registry_;
        notifier_ = std::make_unique<chat::Notifier>(*reactor_, store, sink_, clock_, random_,
                                                     kNode, limits);
        notifier_->start();
        pump();
    }

    // Runs what is due now, and what that makes due now, a few rounds over.
    void pump() {
        for (int i = 0; i < 8; ++i) {
            reactor_->run_once(core::Millis{0});
        }
    }
    void wait(core::Millis d) {
        clock_.advance(d);
        pump();
    }
    // Past `d`, by the reactor's timer resolution (100 ms ticks): what is due by then has run.
    void wait_past(core::Millis d) { wait(d + core::Millis{100}); }

    std::vector<SocketDevice> connected(std::vector<core::UserId> users) {
        std::optional<std::vector<SocketDevice>> out;
        registry_->connected(std::move(users),
                             [&](core::ports::NotifyResult<std::vector<SocketDevice>> r) noexcept {
                                 out = r.value_or(std::vector<SocketDevice>{});
                             });
        pump();
        EXPECT_TRUE(out);
        return out.value_or(std::vector<SocketDevice>{});
    }

    chat::CallEvent ringing(const chat::CallId& call, std::vector<core::UserId> callees) {
        return chat::CallEvent{.event = chat::RingEvent::Ringing,
                               .room = room_id(),
                               .call = call,
                               .kind = chat::CallKind::Direct,
                               .caller = user("alice"),
                               .users = std::move(callees),
                               .by = std::nullopt,
                               .expires_at = clock_.wall_now() + core::Millis{45'000}};
    }
    chat::CallEvent answered(const chat::CallId& call) {
        chat::CallEvent e = ringing(call, {});
        e.event = chat::RingEvent::Answered;
        e.by = user("bob");
        return e;
    }

    ulw::test::FakeClock clock_;
    ulw::test::FakeRandom random_;
    std::unique_ptr<net::IReactor> reactor_;
    std::unique_ptr<infra::notify::MemoryConnectivity> registry_;
    std::unique_ptr<StallingStore> stalling_;
    CollectingSink sink_;
    std::unique_ptr<chat::Notifier> notifier_;
};

TEST_F(NotifierTest, SocketsAreWrittenTogetherAfterAMomentAndOneOpenedAndClosedMeanwhileNever) {
    make();
    ASSERT_TRUE(notifier_->renewed());
    notifier_->opened(user("bob"), device(kPhone));
    notifier_->opened(user("bob"), std::nullopt);
    notifier_->opened(user("bob"), device(kLaptop));
    notifier_->closed(user("bob"), device(kLaptop));
    // Not yet: changes wait for flush_after, with whatever else comes.
    pump();
    EXPECT_TRUE(connected({user("bob")}).empty());
    EXPECT_EQ(notifier_->unwritten(), 3U);
    wait_past(chat::NotifyLimits{}.flush_after);
    const auto bob = connected({user("bob")});
    EXPECT_EQ(bob.size(), 2U);
    EXPECT_NE(std::ranges::find(bob, SocketDevice{.user = user("bob"), .device = device(kPhone)}),
              bob.end());
    EXPECT_NE(std::ranges::find(bob, SocketDevice{.user = user("bob"), .device = std::nullopt}),
              bob.end());
    EXPECT_EQ(notifier_->unwritten(), 0U);
    EXPECT_EQ(notifier_->sockets(), 2U);
    EXPECT_EQ(notifier_->counters().syncs, 1U);
    // Two sockets of one device: closing one leaves it listed.
    notifier_->opened(user("bob"), device(kPhone));
    notifier_->closed(user("bob"), device(kPhone));
    wait_past(chat::NotifyLimits{}.flush_after);
    EXPECT_EQ(connected({user("bob")}).size(), 2U);
    notifier_->closed(user("bob"), device(kPhone));
    notifier_->closed(user("bob"), std::nullopt);
    wait_past(chat::NotifyLimits{}.flush_after);
    EXPECT_TRUE(connected({user("bob")}).empty());
    EXPECT_EQ(notifier_->sockets(), 0U);
}

TEST_F(NotifierTest, ARingingCallNamesEachCalleesConnectedDevicesAndItsLaterEventsFollowIt) {
    make();
    notifier_->opened(user("bob"), device(kPhone));
    notifier_->opened(user("bob"), std::nullopt);
    wait_past(chat::NotifyLimits{}.flush_after);
    const chat::CallId call = chat::CallId::generate(clock_, random_);
    notifier_->observed(ringing(call, {user("bob"), user("carol")}));
    // The registry has not answered: the answer waits behind the ring.
    notifier_->observed(answered(call));
    EXPECT_TRUE(sink_.events.empty());
    pump();
    const auto events = sink_.take();
    ASSERT_EQ(events.size(), 2U);
    const core::json::Value& ring = events[0];
    EXPECT_EQ(text(ring, "type"), "call_ringing");
    EXPECT_EQ(text(ring, "room"), kRoom);
    EXPECT_EQ(text(ring, "call"), call.to_string());
    EXPECT_EQ(text(ring, "kind"), "direct");
    EXPECT_EQ(text(ring, "caller"), "alice");
    EXPECT_FALSE(text(ring, "id").empty());
    EXPECT_NE(text(ring, "id"), text(events[1], "id"));
    ASSERT_NE(ring.find("expires_at"), nullptr);
    EXPECT_EQ(ring.find("expires_at")->as_i64(),
              std::chrono::duration_cast<core::Seconds>(
                  (clock_.wall_now() + core::Millis{45'000}).time_since_epoch())
                  .count());
    const auto* callees = ring.find("callees")->as_array();
    ASSERT_NE(callees, nullptr);
    ASSERT_EQ(callees->size(), 2U);
    EXPECT_EQ(text((*callees)[0], "user"), "bob");
    EXPECT_EQ((*callees)[0].find("connected")->as_bool(), true);
    EXPECT_EQ(strings((*callees)[0].find("devices")),
              std::vector<std::string>{std::string(kPhone)});
    EXPECT_EQ(text((*callees)[1], "user"), "carol");
    EXPECT_EQ((*callees)[1].find("connected")->as_bool(), false);
    EXPECT_TRUE(strings((*callees)[1].find("devices")).empty());
    EXPECT_EQ(text(events[1], "type"), "call_answered");
    EXPECT_EQ(text(events[1], "by"), "bob");
    EXPECT_EQ(text(events[1], "call"), call.to_string());
    // A call no lookup waits on goes at once.
    notifier_->observed(answered(chat::CallId::generate(clock_, random_)));
    EXPECT_EQ(sink_.events.size(), 1U);
    EXPECT_EQ(sink_.take()[0].find("reason"), nullptr);
    // A ring whose caller left the call says so (ADR-0113).
    chat::CallEvent cancelled = answered(chat::CallId::generate(clock_, random_));
    cancelled.event = chat::RingEvent::Cancelled;
    cancelled.by.reset();
    cancelled.reason = chat::CallReason::CallerLeft;
    notifier_->observed(cancelled);
    const auto gone = sink_.take();
    ASSERT_EQ(gone.size(), 1U);
    EXPECT_EQ(text(gone[0], "type"), "call_cancelled");
    EXPECT_EQ(text(gone[0], "reason"), "caller_left");
}

TEST_F(NotifierTest, ARegistryThatDoesNotAnswerInTimeLeavesTheDevicesUnknownRatherThanTheRingLate) {
    make(/*stall=*/true);
    const chat::CallId call = chat::CallId::generate(clock_, random_);
    notifier_->observed(ringing(call, {user("bob")}));
    notifier_->observed(answered(call));
    wait(chat::NotifyLimits{}.lookup_timeout - core::Millis{1});
    EXPECT_TRUE(sink_.events.empty());
    wait(core::Millis{1});
    auto events = sink_.take();
    ASSERT_EQ(events.size(), 2U);
    const auto* callees = events[0].find("callees")->as_array();
    ASSERT_NE(callees, nullptr);
    ASSERT_EQ(callees->size(), 1U);
    EXPECT_TRUE((*callees)[0].find("connected")->is_null());
    EXPECT_TRUE((*callees)[0].find("devices")->is_null());
    EXPECT_EQ(text(events[1], "type"), "call_answered");
    EXPECT_EQ(notifier_->counters().lookups_unknown, 1U);
    // The late answer finds nothing waiting, and sends nothing again.
    ASSERT_EQ(stalling_->stalled.size(), 1U);
    stalling_->stalled[0](std::vector<SocketDevice>{});
    pump();
    EXPECT_TRUE(sink_.events.empty());
}

// ADR-0126: with ULW_NOTIFY_MESSAGE_PRESENT, those with a socket in view are named too, with
// the devices it names, so the receiver pushes to their other devices.
TEST_F(NotifierTest, WithPresentOnAMessageAlsoNamesThePresentAndTheirDevices) {
    chat::NotifyLimits limits;
    limits.message_present = true;
    make(false, limits);
    registry_->set_room(room_id(), core::ports::RoomKind::GroupChat,
                        {user("alice"), user("bob"), user("carol"), user("dave")});
    notifier_->opened(user("carol"), device(kLaptop));
    notifier_->opened(user("carol"), std::nullopt);
    notifier_->opened(user("dave"), std::nullopt);
    wait_past(chat::NotifyLimits{}.flush_after);
    notifier_->message(room_id(), user("alice"), 7);
    pump();
    ASSERT_EQ(sink_.events.size(), 1U);
    const std::string raw = sink_.events[0];
    auto events = sink_.take();
    EXPECT_EQ(strings(events[0].find("users")), std::vector<std::string>{"bob"});
    EXPECT_NE(raw.find(std::string(R"("present":[{"user":"carol","devices":[")") +
                       std::string(kLaptop) + R"("]},{"user":"dave","devices":[]}]})"),
              std::string::npos)
        << raw;
    // Once per interval, present or not.
    notifier_->message(room_id(), user("alice"), 8);
    pump();
    EXPECT_TRUE(sink_.take().empty());
}

TEST_F(NotifierTest, WithoutPresentAMessageNamesNobodyPresent) {
    make();
    registry_->set_room(room_id(), core::ports::RoomKind::GroupChat, {user("alice"), user("bob")});
    notifier_->opened(user("bob"), device(kLaptop));
    wait_past(chat::NotifyLimits{}.flush_after);
    notifier_->message(room_id(), user("alice"), 7);
    pump();
    EXPECT_TRUE(sink_.take().empty());
}

TEST_F(NotifierTest, AMessageNamesTheMembersWithNoSocketAndTellsEachOncePerInterval) {
    make();
    registry_->set_room(room_id(), core::ports::RoomKind::GroupChat,
                        {user("alice"), user("bob"), user("carol")});
    notifier_->opened(user("carol"), std::nullopt);
    wait_past(chat::NotifyLimits{}.flush_after);
    notifier_->message(room_id(), user("alice"), 41);
    pump();
    auto events = sink_.take();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(text(events[0], "type"), "message");
    EXPECT_EQ(text(events[0], "room"), kRoom);
    EXPECT_EQ(text(events[0], "sender"), "alice");
    EXPECT_EQ(events[0].find("seq")->as_u64(), 41U);
    EXPECT_EQ(strings(events[0].find("users")), std::vector<std::string>{"bob"});
    EXPECT_EQ(events[0].find("body"), nullptr) << "never content";
    // Within the interval bob was told already; alice has no socket here either, and is owed
    // carol's message.
    notifier_->message(room_id(), user("carol"), 42);
    pump();
    events = sink_.take();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(strings(events[0].find("users")), std::vector<std::string>{"alice"});
    notifier_->message(room_id(), user("carol"), 43);
    pump();
    EXPECT_TRUE(sink_.take().empty());
    // A stream's live chat is never looked up.
    const auto live = chat::live_chat_room("stream-1");
    ASSERT_TRUE(live);
    registry_->set_room(*live, core::ports::RoomKind::StreamLiveChat, {user("bob")});
    notifier_->message(*live, user("alice"), 1);
    pump();
    EXPECT_TRUE(sink_.take().empty());
}

TEST_F(NotifierTest, ARegistryThatForgotTheNodeIsWrittenEverySocketAgain) {
    make();
    notifier_->opened(user("bob"), device(kPhone));
    wait_past(chat::NotifyLimits{}.flush_after);
    ASSERT_EQ(connected({user("bob")}).size(), 1U);
    bool forgot = false;
    registry_->forget(kNode, [&](core::ports::NotifyResult<void>) noexcept { forgot = true; });
    pump();
    ASSERT_TRUE(forgot);
    EXPECT_TRUE(connected({user("bob")}).empty());
    wait_past(chat::NotifyLimits{}.renew_every);
    EXPECT_EQ(notifier_->counters().resyncs, 2U) << "the first renewal, and this one";
    EXPECT_EQ(connected({user("bob")}).size(), 1U);
}

TEST_F(NotifierTest, AStoreThatFailsIsAskedAgainAndARingStillGoesOut) {
    make();
    registry_->fail(true);
    notifier_->opened(user("bob"), device(kPhone));
    wait_past(chat::NotifyLimits{}.flush_after);
    EXPECT_EQ(notifier_->counters().sync_failures, 1U);
    EXPECT_EQ(notifier_->unwritten(), 1U);
    const chat::CallId call = chat::CallId::generate(clock_, random_);
    notifier_->observed(ringing(call, {user("bob")}));
    pump();
    auto events = sink_.take();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_TRUE((*events[0].find("callees")->as_array())[0].find("devices")->is_null());
    registry_->fail(false);
    wait_past(chat::NotifyLimits{}.retry_after);
    EXPECT_EQ(notifier_->unwritten(), 0U);
    EXPECT_EQ(connected({user("bob")}).size(), 1U);
}

TEST_F(NotifierTest, StoppingForgetsTheNodesSocketsAndSendsWhatWaited) {
    make(/*stall=*/true);
    notifier_->opened(user("bob"), device(kPhone));
    wait_past(chat::NotifyLimits{}.flush_after);
    ASSERT_EQ(connected({user("bob")}).size(), 1U);
    notifier_->observed(ringing(chat::CallId::generate(clock_, random_), {user("bob")}));
    bool done = false;
    notifier_->stop([&]() noexcept { done = true; });
    EXPECT_EQ(sink_.events.size(), 1U) << "the ring waiting on the registry";
    pump();
    EXPECT_TRUE(done);
    EXPECT_TRUE(connected({user("bob")}).empty());
    // Nothing after: the node is going.
    notifier_->opened(user("carol"), std::nullopt);
    notifier_->observed(answered(chat::CallId::generate(clock_, random_)));
    wait_past(chat::NotifyLimits{}.flush_after);
    EXPECT_TRUE(connected({user("carol")}).empty());
    EXPECT_EQ(sink_.events.size(), 1U);
}

} // namespace
