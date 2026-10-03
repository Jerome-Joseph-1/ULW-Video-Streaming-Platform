#include "core/models/ids.hpp"
#include "net/reactor_factory.hpp"

#include "live_fakes.hpp"
#include "live_streams.hpp"
#include "livekit_webhook.hpp"
#include "ops/log.hpp"
#include "publisher_watch.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"
#include "support/memory_log.hpp"
#include "support/reactor_harness.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

using core::ports::LiveEnd;
using core::ports::LiveState;
using core::ports::LiveStream;
using core::ports::MediaError;
using core::ports::PackagerError;
using core::ports::PackagerState;
using gateway::LiveFailure;
using gateway::LiveSettings;
using gateway::LiveStreams;
using gateway::PublisherWatch;
using gateway::WatchSettings;
using gateway::WebhookEvent;
using gateway::WebhookEventKind;
using ulw::test::FakeClock;
using ulw::test::FakeLiveStore;
using ulw::test::FakePackagers;
using ulw::test::FakeRandom;
using ulw::test::FakeSfu;
using ulw::test::MemoryLog;
using ulw::test::pump_pending;
using ulw::test::pump_until;

constexpr core::Millis kGrace{10'000};
constexpr core::Millis kRetry{2'000};

class PublisherWatchTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto r = net::make_reactor(net::ReactorKind::Epoll, clock, 64);
        ASSERT_TRUE(r);
        reactor = std::move(*r);
        store = std::make_unique<FakeLiveStore>(*reactor);
        sfu = std::make_unique<FakeSfu>(*reactor);
        packagers = std::make_unique<FakePackagers>(*reactor);
        live = std::make_unique<LiveStreams>(gateway::LiveDeps{.reactor = *reactor,
                                                               .store = *store,
                                                               .sfu = *sfu,
                                                               .packagers = *packagers,
                                                               .clock = clock,
                                                               .random = random,
                                                               .log = log},
                                             streams());
        watch = std::make_unique<PublisherWatch>(
            *reactor, *live, log, WatchSettings{.grace = kGrace, .retry = kRetry, .attempts = 3});
    }

    void TearDown() override {
        watch.reset();
        live.reset();
        packagers.reset();
        sfu.reset();
        store.reset();
        reactor.reset();
    }

    // Room for the few owners these tests start streams for.
    static LiveSettings streams() {
        LiveSettings settings;
        settings.max_streams = 8;
        return settings;
    }

    // A stream of `owner`'s, Starting, as POST /api/v1/live leaves it.
    core::LiveStreamId create(std::string_view owner = "alice") {
        std::optional<std::expected<gateway::StartedStream, LiveFailure>> r;
        live->create(*core::UserId::parse(owner), [&](auto x) noexcept { r = std::move(x); });
        EXPECT_TRUE(pump_until(*reactor, [&] { return r.has_value(); }));
        return r->value().stream.id;
    }

    // Live through the owner's POST .../start.
    core::LiveStreamId create_live(std::string_view owner = "alice") {
        const auto id = create(owner);
        std::optional<std::expected<LiveStream, LiveFailure>> r;
        live->go_live(id, *core::UserId::parse(owner), [&](auto x) noexcept { r = std::move(x); });
        EXPECT_TRUE(pump_until(*reactor, [&] { return r.has_value(); }));
        EXPECT_TRUE(*r);
        return id;
    }

    static WebhookEvent event(WebhookEventKind kind, const core::LiveStreamId& id,
                              std::string_view session = "PA_one",
                              std::string_view owner = "alice") {
        return WebhookEvent{.kind = kind,
                            .id = "EV_x",
                            .room = id.to_string() + ":1",
                            .identity = std::string(owner) + "/" + id.to_string(),
                            .participant_sid = std::string(session)};
    }

    void joined(const core::LiveStreamId& id, std::string_view session = "PA_one") {
        watch->on_event(event(WebhookEventKind::ParticipantJoined, id, session));
    }
    void left(const core::LiveStreamId& id, std::string_view session = "PA_one") {
        watch->on_event(event(WebhookEventKind::ParticipantLeft, id, session));
    }

    // Moves time on and runs whatever it made due, and whatever that set off.
    void advance(core::Millis d) {
        clock.advance(d);
        pump_pending(*reactor);
    }

    LiveStream& row(const core::LiveStreamId& id) { return store->rows.at(id.to_string()); }

    [[nodiscard]] std::size_t count_calls(std::string_view prefix) const {
        return static_cast<std::size_t>(std::ranges::count_if(
            sfu->calls, [&](const std::string& c) { return c.starts_with(prefix); }));
    }

    FakeClock clock;
    FakeRandom random;
    MemoryLog sink;
    ops::Logger log{sink, clock, "gateway", ops::Level::Debug};
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<FakeLiveStore> store;
    std::unique_ptr<FakeSfu> sfu;
    std::unique_ptr<FakePackagers> packagers;
    std::unique_ptr<LiveStreams> live;
    std::unique_ptr<PublisherWatch> watch;
};

TEST_F(PublisherWatchTest, APublisherJoiningTakesTheStreamLiveThroughTheStartPath) {
    const auto id = create();
    joined(id);
    ASSERT_TRUE(pump_until(*reactor, [&] { return live->counters().went_live == 1U; }));
    EXPECT_EQ(row(id).state, LiveState::Live);
    ASSERT_EQ(packagers->started.size(), 1U);
    EXPECT_EQ(packagers->started[0].owner, *core::UserId::parse("alice"));
    EXPECT_EQ(count_calls("relay " + id.to_string()), 1U);
    EXPECT_EQ(watch->counters().starts, 1U);
}

TEST_F(PublisherWatchTest, ThePublishersRepeatedEventsStartItOnce) {
    const auto id = create();
    // participant_joined and a track_published for each track, and one delivered twice.
    joined(id);
    watch->on_event(event(WebhookEventKind::TrackPublished, id));
    watch->on_event(event(WebhookEventKind::TrackPublished, id));
    ASSERT_TRUE(pump_until(*reactor, [&] { return row(id).state == LiveState::Live; }));
    joined(id);
    watch->on_event(event(WebhookEventKind::TrackPublished, id));
    pump_pending(*reactor);
    EXPECT_EQ(packagers->started.size(), 1U);
    EXPECT_EQ(watch->counters().starts, 1U);
}

TEST_F(PublisherWatchTest, TheOwnersStartAndAWebhookTogetherMakeOneStream) {
    const auto id = create();
    std::optional<std::expected<LiveStream, LiveFailure>> api;
    live->go_live(id, *core::UserId::parse("alice"), [&](auto x) noexcept { api = std::move(x); });
    joined(id);
    ASSERT_TRUE(pump_until(*reactor, [&] { return api.has_value(); }));
    ASSERT_TRUE(*api);
    EXPECT_EQ((*api)->state, LiveState::Live);
    pump_pending(*reactor);
    EXPECT_EQ(row(id).state, LiveState::Live);
    // The packager runtime and the relay are idempotent: one packager and one relay.
    EXPECT_EQ(sfu->relays.size(), 1U);
    EXPECT_EQ(packagers->states.size(), 1U);
    // And the owner's start afterwards answers the same stream.
    std::optional<std::expected<LiveStream, LiveFailure>> again;
    live->go_live(id, *core::UserId::parse("alice"),
                  [&](auto x) noexcept { again = std::move(x); });
    ASSERT_TRUE(pump_until(*reactor, [&] { return again.has_value(); }));
    ASSERT_TRUE(*again);
    EXPECT_EQ(sfu->relays.size(), 1U);
}

TEST_F(PublisherWatchTest, APublisherWhoLeftEndsTheStreamOnceTheGraceRunsOut) {
    const auto id = create_live();
    left(id);
    EXPECT_TRUE(watch->in_grace(id));
    advance(kGrace - core::Millis{1});
    EXPECT_EQ(row(id).state, LiveState::Live) << "ended inside the grace";
    EXPECT_EQ(count_calls("present"), 0U);
    advance(core::Millis{1});
    ASSERT_TRUE(pump_until(*reactor, [&] { return row(id).state == LiveState::Ended; }));
    EXPECT_EQ(row(id).ended_by, LiveEnd::PublisherLeft);
    EXPECT_EQ(row(id).ended_at, clock.wall_now());
    // The media server was asked first, and the room closed after.
    const std::string name = id.to_string();
    EXPECT_EQ(count_calls("present " + name + ":1 alice/" + name), 1U);
    ASSERT_TRUE(pump_until(*reactor, [&] { return count_calls("close " + name) == 1U; }));
    EXPECT_EQ(watch->counters().ended, 1U);
    EXPECT_EQ(live->counters().ended.at(static_cast<std::size_t>(LiveEnd::PublisherLeft)), 1U);
    EXPECT_FALSE(watch->in_grace(id));
    EXPECT_EQ(watch->followed(), 0U);
}

TEST_F(PublisherWatchTest, APublisherBackWithinTheGraceKeepsTheStream) {
    const auto id = create_live();
    joined(id, "PA_one");
    pump_pending(*reactor);
    left(id, "PA_one");
    advance(kGrace / 2);
    // A full reconnect: a new session.
    joined(id, "PA_two");
    EXPECT_FALSE(watch->in_grace(id));
    advance(kGrace);
    advance(kGrace);
    EXPECT_EQ(row(id).state, LiveState::Live);
    EXPECT_EQ(count_calls("present"), 0U);
    EXPECT_EQ(watch->counters().returns, 1U);
    // Gone again for good: a grace of its own, from then.
    left(id, "PA_two");
    advance(kGrace);
    ASSERT_TRUE(pump_until(*reactor, [&] { return row(id).state == LiveState::Ended; }));
    EXPECT_EQ(row(id).ended_by, LiveEnd::PublisherLeft);
}

TEST_F(PublisherWatchTest, ALateJoinOfASessionSeenLeavingChangesNothing) {
    const auto id = create_live();
    // Delivered out of order: the leave first, then its own join.
    left(id, "PA_one");
    joined(id, "PA_one");
    watch->on_event(event(WebhookEventKind::TrackPublished, id, "PA_one"));
    EXPECT_EQ(watch->counters().stale, 2U);
    EXPECT_TRUE(watch->in_grace(id));
    advance(kGrace);
    ASSERT_TRUE(pump_until(*reactor, [&] { return row(id).state == LiveState::Ended; }));
    EXPECT_EQ(row(id).ended_by, LiveEnd::PublisherLeft);
    EXPECT_TRUE(packagers->started.size() == 1U) << "the late join started nothing";
}

TEST_F(PublisherWatchTest, LeavesDeliveredTwiceStartOneGrace) {
    const auto id = create_live();
    left(id);
    advance(kGrace / 2);
    left(id);
    watch->on_event(event(WebhookEventKind::ParticipantAborted, id));
    EXPECT_EQ(watch->counters().departures, 1U);
    // The grace counts from the first.
    advance(kGrace / 2);
    ASSERT_TRUE(pump_until(*reactor, [&] { return row(id).state == LiveState::Ended; }));
}

TEST_F(PublisherWatchTest, TheMediaServerHasTheLastWord) {
    const auto id = create_live();
    left(id);
    // Back through another replica, which this one never heard of: LiveKit still has it.
    sfu->connected.insert("alice/" + id.to_string());
    advance(kGrace);
    ASSERT_TRUE(pump_until(*reactor, [&] { return watch->counters().kept == 1U; }));
    EXPECT_EQ(row(id).state, LiveState::Live);
    EXPECT_EQ(count_calls("close"), 0U);
}

TEST_F(PublisherWatchTest, ARoomThatFinishedEndsItsLiveStream) {
    const auto id = create_live();
    joined(id);
    pump_pending(*reactor);
    watch->on_event(WebhookEvent{
        .kind = WebhookEventKind::RoomFinished, .id = "EV_r", .room = id.to_string() + ":1"});
    EXPECT_TRUE(watch->in_grace(id));
    // A late join of the session the room took with it changes nothing either.
    joined(id);
    EXPECT_TRUE(watch->in_grace(id));
    advance(kGrace);
    ASSERT_TRUE(pump_until(*reactor, [&] { return row(id).state == LiveState::Ended; }));
    EXPECT_EQ(row(id).ended_by, LiveEnd::PublisherLeft);
}

TEST_F(PublisherWatchTest, AStreamStillStartingKeepsItsStartWindow) {
    const auto id = create();
    // Its room comes and goes with the tickets while the broadcaster sets up.
    watch->on_event(WebhookEvent{
        .kind = WebhookEventKind::RoomFinished, .id = "EV_r", .room = id.to_string() + ":1"});
    advance(kGrace);
    pump_pending(*reactor);
    EXPECT_EQ(row(id).state, LiveState::Starting);
    EXPECT_EQ(count_calls("present"), 0U);
    // And it still goes live when the publisher comes.
    joined(id, "PA_new");
    ASSERT_TRUE(pump_until(*reactor, [&] { return row(id).state == LiveState::Live; }));
}

TEST_F(PublisherWatchTest, TheOwnersEndDuringTheGraceStands) {
    const auto id = create_live();
    left(id);
    std::optional<std::expected<LiveStream, LiveFailure>> ended;
    live->end(id, *core::UserId::parse("alice"), [&](auto x) noexcept { ended = std::move(x); });
    ASSERT_TRUE(pump_until(*reactor, [&] { return ended.has_value(); }));
    advance(kGrace);
    pump_pending(*reactor);
    EXPECT_EQ(row(id).ended_by, LiveEnd::Owner);
    EXPECT_EQ(watch->counters().ended, 0U);
    // A late join afterwards starts nothing again.
    joined(id, "PA_late");
    pump_pending(*reactor);
    EXPECT_EQ(packagers->started.size(), 1U);
    EXPECT_EQ(row(id).ended_by, LiveEnd::Owner);
}

TEST_F(PublisherWatchTest, EventsAboutNoStreamsPublisherAreOnlyCounted) {
    const auto id = create();
    const std::string room = id.to_string() + ":1";
    for (const WebhookEvent& e : {
             // LiveKit's recorder in the stream's room, and another user claiming the stream.
             WebhookEvent{.kind = WebhookEventKind::ParticipantJoined,
                          .room = room,
                          .identity = "EG_recorder"},
             WebhookEvent{.kind = WebhookEventKind::ParticipantLeft,
                          .room = room,
                          .identity = "EG_recorder"},
             // A call's room and its member.
             WebhookEvent{.kind = WebhookEventKind::ParticipantJoined,
                          .room = id.to_string() + ":2",
                          .identity = "alice/" + id.to_string()},
             WebhookEvent{.kind = WebhookEventKind::RoomFinished, .room = "call-room:4"},
             WebhookEvent{.kind = WebhookEventKind::Other, .room = room},
         }) {
        watch->on_event(e);
    }
    pump_pending(*reactor);
    EXPECT_EQ(watch->counters().ignored, 5U);
    EXPECT_EQ(watch->followed(), 0U);
    EXPECT_TRUE(packagers->started.empty());
    EXPECT_EQ(row(id).state, LiveState::Starting);
}

TEST_F(PublisherWatchTest, AStreamTheIdentityDoesNotOwnOrThatDoesNotExistIsLetGo) {
    const auto id = create("alice");
    // Signed by LiveKit, but naming bob as the publisher of alice's stream.
    watch->on_event(event(WebhookEventKind::ParticipantJoined, id, "PA_b", "bob"));
    // A stream id nobody stored, as from another environment sharing the LiveKit.
    const auto unknown = core::LiveStreamId::generate(clock, random);
    joined(unknown);
    pump_pending(*reactor);
    EXPECT_TRUE(packagers->started.empty());
    EXPECT_EQ(row(id).state, LiveState::Starting);
    EXPECT_EQ(watch->followed(), 0U);
    // Their departures cost a look at the row and nothing more.
    left(unknown);
    advance(kGrace);
    pump_pending(*reactor);
    EXPECT_EQ(count_calls("present"), 0U);
    EXPECT_EQ(watch->followed(), 0U);
}

TEST_F(PublisherWatchTest, AStartRefusedForNowIsTriedAgainWhileThePublisherStays) {
    const auto id = create();
    packagers->fail_start = PackagerError::Unavailable;
    joined(id);
    ASSERT_TRUE(pump_until(*reactor, [&] { return watch->counters().start_failures == 1U; }));
    EXPECT_EQ(row(id).state, LiveState::Starting);
    packagers->fail_start.reset();
    advance(kRetry);
    ASSERT_TRUE(pump_until(*reactor, [&] { return row(id).state == LiveState::Live; }));
    EXPECT_EQ(watch->counters().starts, 2U);
}

TEST_F(PublisherWatchTest, StartRetriesStopAtTheirLimitAndWhenThePublisherGoes) {
    const auto id = create();
    packagers->fail_start = PackagerError::Unavailable;
    joined(id);
    for (int i = 0; i < 5; ++i) {
        pump_pending(*reactor);
        advance(kRetry);
    }
    EXPECT_EQ(watch->counters().starts, 3U);
    // Refused as made: never retried.
    const auto other = create("bob");
    packagers->fail_start = PackagerError::Refused;
    watch->on_event(event(WebhookEventKind::ParticipantJoined, other, "PA_b", "bob"));
    for (int i = 0; i < 3; ++i) {
        pump_pending(*reactor);
        advance(kRetry);
    }
    EXPECT_EQ(watch->counters().starts, 4U);
}

TEST_F(PublisherWatchTest, AGraceThatRunsOutDuringAStartWaitsForIt) {
    const auto id = create();
    packagers->started_state = PackagerState::Starting;
    joined(id);
    ASSERT_TRUE(pump_until(*reactor, [&] { return packagers->started.size() == 1U; }));
    left(id);
    advance(kGrace);
    EXPECT_EQ(count_calls("present"), 0U) << "looked while the start could still mark it live";
    packagers->states.at(id.to_string()) = PackagerState::Ready;
    advance(LiveSettings{}.ready_poll);
    ASSERT_TRUE(pump_until(*reactor, [&] { return row(id).state == LiveState::Ended; }));
    EXPECT_EQ(row(id).ended_by, LiveEnd::PublisherLeft);
}

TEST_F(PublisherWatchTest, ACheckTheMediaServerCouldNotAnswerIsTriedAgain) {
    const auto id = create_live();
    sfu->fail_present = MediaError::Unavailable;
    left(id);
    advance(kGrace);
    ASSERT_TRUE(pump_until(*reactor, [&] { return watch->counters().check_failures == 1U; }));
    EXPECT_EQ(row(id).state, LiveState::Live);
    sfu->fail_present.reset();
    advance(kRetry);
    ASSERT_TRUE(pump_until(*reactor, [&] { return row(id).state == LiveState::Ended; }));
    EXPECT_EQ(row(id).ended_by, LiveEnd::PublisherLeft);
}

TEST_F(PublisherWatchTest, ChecksGiveUpAtTheirLimitAndLeaveTheStreamToTheSweep) {
    const auto id = create_live();
    sfu->fail_present = MediaError::Unavailable;
    left(id);
    advance(kGrace);
    for (int i = 0; i < 5; ++i) {
        pump_pending(*reactor);
        advance(kRetry);
    }
    EXPECT_EQ(watch->counters().check_failures, 3U);
    EXPECT_EQ(row(id).state, LiveState::Live);
    EXPECT_FALSE(watch->in_grace(id));
}

TEST_F(PublisherWatchTest, OnlyIdleStreamsMakeRoomForMore) {
    watch = std::make_unique<PublisherWatch>(
        *reactor, *live, log,
        WatchSettings{.grace = kGrace, .retry = kRetry, .attempts = 3, .max_streams = 2});
    const auto a = create_live("alice");
    const auto b = create_live("bob");
    const auto c = create_live("carol");
    // Two graces running: neither may be dropped, so a third stream is not followed.
    watch->on_event(event(WebhookEventKind::ParticipantLeft, a, "PA_a", "alice"));
    watch->on_event(event(WebhookEventKind::ParticipantLeft, b, "PA_b", "bob"));
    watch->on_event(event(WebhookEventKind::ParticipantLeft, c, "PA_c", "carol"));
    EXPECT_EQ(watch->counters().untracked, 1U);
    EXPECT_EQ(watch->followed(), 2U);
    advance(kGrace);
    ASSERT_TRUE(pump_until(*reactor, [&] { return watch->followed() == 0U; }));
    EXPECT_EQ(row(a).state, LiveState::Ended);
    EXPECT_EQ(row(b).state, LiveState::Ended);
    EXPECT_EQ(row(c).state, LiveState::Live);
}

TEST_F(PublisherWatchTest, DestroyingTheWatchWithCallsUnderWayIsSafe) {
    const auto id = create();
    joined(id);
    watch.reset();
    ASSERT_TRUE(pump_until(*reactor, [&] { return row(id).state == LiveState::Live; }));
    pump_pending(*reactor);
}

} // namespace
