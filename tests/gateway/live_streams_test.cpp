#include "core/models/ids.hpp"
#include "net/reactor_factory.hpp"

#include "live_fakes.hpp"
#include "live_streams.hpp"
#include "ops/log.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"
#include "support/memory_log.hpp"
#include "support/reactor_harness.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using core::ports::LiveEnd;
using core::ports::LiveState;
using core::ports::LiveStoreError;
using core::ports::LiveStream;
using core::ports::MediaError;
using core::ports::MediaTicket;
using core::ports::PackagerError;
using core::ports::PackagerState;
using gateway::LiveFailure;
using gateway::LiveSettings;
using gateway::LiveStreams;
using gateway::StartedStream;
using ulw::test::FakeClock;
using ulw::test::FakeLiveStore;
using ulw::test::FakePackagers;
using ulw::test::FakeRandom;
using ulw::test::FakeSfu;
using ulw::test::MemoryLog;
using ulw::test::pump_until;

core::UserId user(std::string_view name) {
    return *core::UserId::parse(name);
}

template <class T> using Result = std::optional<std::expected<T, LiveFailure>>;

class LiveStreamsTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto r = net::make_reactor(net::ReactorKind::Epoll, clock, 64);
        ASSERT_TRUE(r);
        reactor = std::move(*r);
        store = std::make_unique<FakeLiveStore>(*reactor);
        sfu = std::make_unique<FakeSfu>(*reactor);
        packagers = std::make_unique<FakePackagers>(*reactor);
        make(LiveSettings{});
    }

    void TearDown() override {
        live.reset();
        packagers.reset();
        sfu.reset();
        store.reset();
        reactor.reset();
    }

    void make(LiveSettings settings) {
        live.reset();
        live = std::make_unique<LiveStreams>(gateway::LiveDeps{.reactor = *reactor,
                                                               .store = *store,
                                                               .sfu = *sfu,
                                                               .packagers = *packagers,
                                                               .clock = clock,
                                                               .random = random,
                                                               .log = log},
                                             settings);
    }

    template <class T> std::expected<T, LiveFailure> wait(Result<T>& result) {
        EXPECT_TRUE(pump_until(*reactor, [&] { return result.has_value(); }));
        if (!result) {
            return std::unexpected(LiveFailure::Internal);
        }
        return std::move(*result);
    }

    std::expected<StartedStream, LiveFailure> create(std::string_view owner) {
        Result<StartedStream> r;
        live->create(user(owner), [&](auto x) noexcept { r = std::move(x); });
        return wait(r);
    }

    std::expected<MediaTicket, LiveFailure> ticket(const core::LiveStreamId& id,
                                                   std::string_view owner) {
        Result<MediaTicket> r;
        live->ticket(id, user(owner), [&](auto x) noexcept { r = std::move(x); });
        return wait(r);
    }

    std::expected<LiveStream, LiveFailure> go_live(const core::LiveStreamId& id,
                                                   std::string_view owner) {
        Result<LiveStream> r;
        live->go_live(id, user(owner), [&](auto x) noexcept { r = std::move(x); });
        return wait(r);
    }

    std::expected<LiveStream, LiveFailure> end(const core::LiveStreamId& id,
                                               std::string_view owner) {
        Result<LiveStream> r;
        live->end(id, user(owner), [&](auto x) noexcept { r = std::move(x); });
        return wait(r);
    }

    std::expected<LiveStream, LiveFailure> status(const core::LiveStreamId& id) {
        Result<LiveStream> r;
        live->status(id, [&](auto x) noexcept { r = std::move(x); });
        return wait(r);
    }

    LiveStream& row(const core::LiveStreamId& id) { return store->rows.at(id.to_string()); }

    // Runs one sweep to its end.
    void sweep() {
        const std::uint64_t before = live->counters().sweeps;
        live->sweep_now();
        ASSERT_EQ(live->counters().sweeps, before + 1);
        ASSERT_TRUE(pump_until(*reactor, [&] { return !live->sweeping(); }));
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
};

TEST_F(LiveStreamsTest, AStreamStartsWithItsFirstPublisherTicket) {
    const auto started = create("alice");
    ASSERT_TRUE(started);
    EXPECT_TRUE(started->created);
    const LiveStream& s = started->stream;
    EXPECT_EQ(s.owner, user("alice"));
    EXPECT_EQ(s.state, LiveState::Starting);
    EXPECT_EQ(s.created_at, clock.wall_now());
    // A secret SRT accepts: 48 hex characters.
    EXPECT_EQ(s.passphrase.size(), 48U);
    EXPECT_EQ(s.passphrase.find_first_not_of("0123456789abcdef"), std::string::npos);
    EXPECT_EQ(started->ticket.endpoint, "https://media.example.test/whip/v1");
    EXPECT_EQ(started->ticket.credential, "ticket-1");
    // The stream's own room, generation 1, a stream's kind with no participant limit; the
    // publisher is the owner with the stream as its device: one identity per stream.
    const std::string id = s.id.to_string();
    EXPECT_EQ(sfu->calls, (std::vector<std::string>{
                              "open " + id + ":1 stream 0",
                              "join " + id + " alice/" + id + " publisher",
                          }));
    EXPECT_EQ(live->counters().created, 1U);
    EXPECT_EQ(live->counters().tickets, 1U);
    EXPECT_TRUE(packagers->started.empty()) << "nothing runs until the owner goes live";
}

TEST_F(LiveStreamsTest, AnOwnerAskingAgainGetsTheirUnfinishedStreamWithAFreshTicket) {
    const auto first = create("alice");
    ASSERT_TRUE(first);
    const auto again = create("alice");
    ASSERT_TRUE(again);
    EXPECT_FALSE(again->created);
    EXPECT_EQ(again->stream.id, first->stream.id);
    EXPECT_EQ(again->ticket.credential, "ticket-2");
    EXPECT_EQ(live->counters().created, 1U);
    // Once it has ended, the next ask is a new stream.
    ASSERT_TRUE(end(first->stream.id, "alice"));
    const auto next = create("alice");
    ASSERT_TRUE(next);
    EXPECT_TRUE(next->created);
    EXPECT_NE(next->stream.id, first->stream.id);
}

TEST_F(LiveStreamsTest, ThePlatformTakesOnlyAsManyStreamsAsConfigured) {
    LiveSettings one;
    one.max_streams = 1;
    make(one);
    ASSERT_TRUE(create("alice"));
    EXPECT_EQ(create("bob"), std::unexpected(LiveFailure::Full));
}

TEST_F(LiveStreamsTest, CreateReportsWhatFailed) {
    store->fail = LiveStoreError::Unavailable;
    EXPECT_EQ(create("alice"), std::unexpected(LiveFailure::Unavailable));
    store->fail = LiveStoreError::Corrupt;
    EXPECT_EQ(create("alice"), std::unexpected(LiveFailure::Internal));
    EXPECT_EQ(live->counters().store_failures, 2U);
    store->fail.reset();
    sfu->fail_open = MediaError::Unavailable;
    EXPECT_EQ(create("alice"), std::unexpected(LiveFailure::Unavailable));
    sfu->fail_open.reset();
    sfu->fail_join = MediaError::Refused;
    EXPECT_EQ(create("alice"), std::unexpected(LiveFailure::Internal));
    EXPECT_EQ(live->counters().media_failures, 2U);
    // The row stands, Starting: asking again finds it and tries the ticket again.
    sfu->fail_join.reset();
    const auto retried = create("alice");
    ASSERT_TRUE(retried);
    EXPECT_FALSE(retried->created);
}

TEST_F(LiveStreamsTest, TicketsAreTheOwnersAloneAndOnlyWhileTheStreamRuns) {
    const auto started = create("alice");
    ASSERT_TRUE(started);
    const core::LiveStreamId id = started->stream.id;
    const auto fresh = ticket(id, "alice");
    ASSERT_TRUE(fresh);
    EXPECT_EQ(fresh->credential, "ticket-2");
    // Someone else's stream reads as none.
    EXPECT_EQ(ticket(id, "bob"), std::unexpected(LiveFailure::NotFound));
    EXPECT_EQ(ticket(core::LiveStreamId::generate(clock, random), "alice"),
              std::unexpected(LiveFailure::NotFound));
    ASSERT_TRUE(end(id, "alice"));
    EXPECT_EQ(ticket(id, "alice"), std::unexpected(LiveFailure::Ended));
    sfu->fail_join = MediaError::Closed;
    const auto other = create("carol");
    EXPECT_EQ(other, std::unexpected(LiveFailure::Ended));
}

TEST_F(LiveStreamsTest, GoingLiveStartsThePackagerWaitsForItAndRelaysThePublisher) {
    LiveSettings settings;
    settings.segment = core::Seconds{4};
    make(settings);
    const auto started = create("alice");
    ASSERT_TRUE(started);
    const LiveStream stream = started->stream;
    const std::string id = stream.id.to_string();
    packagers->started_state = PackagerState::Starting;

    Result<LiveStream> r;
    live->go_live(stream.id, user("alice"), [&](auto x) noexcept { r = std::move(x); });
    ASSERT_TRUE(pump_until(*reactor, [&] { return packagers->state_calls == 1; }));
    ASSERT_EQ(packagers->started.size(), 1U);
    EXPECT_EQ(packagers->started[0].stream, stream.id);
    EXPECT_EQ(packagers->started[0].owner, user("alice"));
    EXPECT_EQ(packagers->started[0].passphrase, stream.passphrase);
    // Not listening yet: it looks again after the poll interval, and relays once it is.
    ulw::test::pump_pending(*reactor);
    EXPECT_FALSE(r.has_value());
    packagers->states[id] = PackagerState::Ready;
    clock.advance(settings.ready_poll);
    const auto live_now = wait(r);
    ASSERT_TRUE(live_now);
    EXPECT_EQ(live_now->state, LiveState::Live);
    EXPECT_EQ(live_now->live_at, clock.wall_now());
    EXPECT_EQ(sfu->calls.back(),
              "relay " + id + " alice/" + id + " " + id + " " + stream.passphrase + " 4");
    EXPECT_EQ(live->counters().went_live, 1U);

    // Again, as a client retrying a lost answer does: the same relay, nothing counted twice.
    clock.advance(core::Millis{1000});
    const auto again = go_live(stream.id, "alice");
    ASSERT_TRUE(again);
    EXPECT_EQ(again->live_at, live_now->live_at);
    EXPECT_EQ(sfu->relays.size(), 1U);
    EXPECT_EQ(live->counters().went_live, 1U);
}

TEST_F(LiveStreamsTest, GoingLiveIsTheOwnersAndNotAfterTheEnd) {
    const auto started = create("alice");
    ASSERT_TRUE(started);
    EXPECT_EQ(go_live(started->stream.id, "bob"), std::unexpected(LiveFailure::NotFound));
    ASSERT_TRUE(end(started->stream.id, "alice"));
    EXPECT_EQ(go_live(started->stream.id, "alice"), std::unexpected(LiveFailure::Ended));
    EXPECT_TRUE(packagers->started.empty());
}

TEST_F(LiveStreamsTest, APackagerThatCannotStartFailsGoingLive) {
    const auto started = create("alice");
    ASSERT_TRUE(started);
    packagers->fail_start = PackagerError::Unavailable;
    EXPECT_EQ(go_live(started->stream.id, "alice"), std::unexpected(LiveFailure::Unavailable));
    packagers->fail_start = PackagerError::Refused;
    EXPECT_EQ(go_live(started->stream.id, "alice"), std::unexpected(LiveFailure::Internal));
    packagers->fail_start.reset();
    packagers->fail_state = PackagerError::Unavailable;
    EXPECT_EQ(go_live(started->stream.id, "alice"), std::unexpected(LiveFailure::Unavailable));
    EXPECT_EQ(live->counters().packager_failures, 3U);
    EXPECT_EQ(row(started->stream.id).state, LiveState::Starting);
}

TEST_F(LiveStreamsTest, APackagerNotListeningInTimeIsUnavailable) {
    LiveSettings settings;
    settings.ready_wait = core::Millis{2000};
    make(settings);
    const auto started = create("alice");
    ASSERT_TRUE(started);
    packagers->started_state = PackagerState::Starting;
    Result<LiveStream> r;
    live->go_live(started->stream.id, user("alice"), [&](auto x) noexcept { r = std::move(x); });
    // Time moves on a tenth of a poll at every loop turn until the wait gives up.
    ASSERT_TRUE(pump_until(*reactor, [&] {
        clock.advance(settings.ready_poll / 10);
        return r.has_value();
    }));
    EXPECT_EQ(*r, std::unexpected(LiveFailure::Unavailable));
    // Asked at the start and once a poll after, until the deadline.
    EXPECT_GE(packagers->state_calls, 4U);
    EXPECT_LE(packagers->state_calls, 6U);
    EXPECT_TRUE(sfu->relays.empty());
}

TEST_F(LiveStreamsTest, APackagerThatHasGoneEndsTheStream) {
    for (const PackagerState gone : {PackagerState::Finished, PackagerState::Failed}) {
        const auto started = create("alice");
        ASSERT_TRUE(started);
        packagers->started_state = gone;
        EXPECT_EQ(go_live(started->stream.id, "alice"), std::unexpected(LiveFailure::Ended));
        const LiveStream& r = row(started->stream.id);
        EXPECT_EQ(r.state, LiveState::Ended);
        EXPECT_EQ(r.ended_by,
                  gone == PackagerState::Finished ? LiveEnd::Finished : LiveEnd::Failed);
    }
}

TEST_F(LiveStreamsTest, ARelayTheMediaServerRefusesLeavesTheStreamStarting) {
    const auto started = create("alice");
    ASSERT_TRUE(started);
    sfu->fail_relay = MediaError::Unavailable;
    EXPECT_EQ(go_live(started->stream.id, "alice"), std::unexpected(LiveFailure::Unavailable));
    sfu->fail_relay = MediaError::Refused;
    EXPECT_EQ(go_live(started->stream.id, "alice"), std::unexpected(LiveFailure::Internal));
    EXPECT_EQ(row(started->stream.id).state, LiveState::Starting);
    sfu->fail_relay.reset();
    sfu->fail_open = MediaError::Unavailable;
    EXPECT_EQ(go_live(started->stream.id, "alice"), std::unexpected(LiveFailure::Unavailable));
    sfu->fail_open.reset();
    // Marking it live fails: the relay stands, and a retry marks it.
    store->fail = LiveStoreError::Unavailable;
    Result<LiveStream> r;
    live->go_live(started->stream.id, user("alice"), [&](auto x) noexcept { r = std::move(x); });
    EXPECT_EQ(wait(r), std::unexpected(LiveFailure::Unavailable));
    store->fail.reset();
    ASSERT_TRUE(go_live(started->stream.id, "alice"));
}

TEST_F(LiveStreamsTest, AStreamEndedWhileGoingLiveIsNotMadeLive) {
    const auto started = create("alice");
    ASSERT_TRUE(started);
    Result<LiveStream> r;
    live->go_live(started->stream.id, user("alice"), [&](auto x) noexcept { r = std::move(x); });
    // The owner's end lands between the relay and the row.
    ASSERT_TRUE(pump_until(*reactor, [&] { return !sfu->relays.empty(); }));
    row(started->stream.id).state = LiveState::Ended;
    row(started->stream.id).ended_at = clock.wall_now();
    row(started->stream.id).ended_by = LiveEnd::Owner;
    EXPECT_EQ(wait(r), std::unexpected(LiveFailure::Ended));
    EXPECT_EQ(row(started->stream.id).state, LiveState::Ended);
}

TEST_F(LiveStreamsTest, TheOwnerEndsTheStreamAndItsRoomIsClosedEveryTime) {
    const auto started = create("alice");
    ASSERT_TRUE(started);
    const std::string id = started->stream.id.to_string();
    EXPECT_EQ(end(started->stream.id, "bob"), std::unexpected(LiveFailure::NotFound));
    const auto ended = end(started->stream.id, "alice");
    ASSERT_TRUE(ended);
    EXPECT_EQ(ended->state, LiveState::Ended);
    EXPECT_EQ(ended->ended_by, LiveEnd::Owner);
    EXPECT_EQ(sfu->calls.back(), "close " + id);
    // A repeat answers the first end and closes the room again, for a close that failed.
    clock.advance(core::Millis{5000});
    sfu->calls.clear();
    const auto again = end(started->stream.id, "alice");
    ASSERT_TRUE(again);
    EXPECT_EQ(again->ended_at, ended->ended_at);
    EXPECT_EQ(sfu->calls.back(), "close " + id);
    EXPECT_EQ(live->counters().ended.at(static_cast<std::size_t>(LiveEnd::Owner)), 1U);
}

TEST_F(LiveStreamsTest, AnEndTheMediaServerMissesIsReportedAndTheStreamIsEndedAnyway) {
    const auto started = create("alice");
    ASSERT_TRUE(started);
    sfu->fail_close = MediaError::Unavailable;
    EXPECT_EQ(end(started->stream.id, "alice"), std::unexpected(LiveFailure::Unavailable));
    EXPECT_EQ(row(started->stream.id).state, LiveState::Ended);
    EXPECT_EQ(ticket(started->stream.id, "alice"), std::unexpected(LiveFailure::Ended));
    sfu->fail_close.reset();
    EXPECT_TRUE(end(started->stream.id, "alice"));
    store->fail = LiveStoreError::Unavailable;
    EXPECT_EQ(end(started->stream.id, "alice"), std::unexpected(LiveFailure::Unavailable));
}

TEST_F(LiveStreamsTest, AnyoneSeesAStreamsStatus) {
    const auto started = create("alice");
    ASSERT_TRUE(started);
    const auto seen = status(started->stream.id);
    ASSERT_TRUE(seen);
    EXPECT_EQ(seen->id, started->stream.id);
    EXPECT_EQ(status(core::LiveStreamId::generate(clock, random)),
              std::unexpected(LiveFailure::NotFound));
}

TEST_F(LiveStreamsTest, AnEndedPlaylistEndsTheRow) {
    const auto started = create("alice");
    ASSERT_TRUE(started);
    ASSERT_TRUE(go_live(started->stream.id, "alice"));
    live->playlist_ended(started->stream.id);
    ASSERT_TRUE(pump_until(*reactor, [&] { return live->pending() == 0; }));
    EXPECT_EQ(row(started->stream.id).state, LiveState::Ended);
    EXPECT_EQ(row(started->stream.id).ended_by, LiveEnd::Finished);
}

TEST_F(LiveStreamsTest, TheSweepEndsWhatHasEndedOrNeverStarted) {
    LiveSettings settings;
    settings.start_window = core::Seconds{600};
    settings.max_age = core::Seconds{3600};
    settings.max_streams = 10;
    make(settings);
    const auto idle = create("idle");
    const auto done = create("done");
    const auto broken = create("broken");
    const auto vanished = create("vanished");
    const auto running = create("running");
    for (const auto* s : {&done, &broken, &vanished, &running}) {
        ASSERT_TRUE(*s);
        ASSERT_TRUE(go_live((*s)->stream.id, (*s)->stream.owner.view()));
    }
    ASSERT_TRUE(idle);
    packagers->states[done->stream.id.to_string()] = PackagerState::Finished;
    packagers->states[broken->stream.id.to_string()] = PackagerState::Failed;
    packagers->states.erase(vanished->stream.id.to_string());

    sweep();
    EXPECT_EQ(row(idle->stream.id).state, LiveState::Starting) << "still inside its window";
    EXPECT_EQ(row(done->stream.id).ended_by, LiveEnd::Finished);
    EXPECT_EQ(row(broken->stream.id).ended_by, LiveEnd::Failed);
    EXPECT_EQ(row(vanished->stream.id).ended_by, LiveEnd::Failed);
    EXPECT_EQ(row(running->stream.id).state, LiveState::Live);

    clock.advance(core::Millis{601'000});
    sweep();
    EXPECT_EQ(row(idle->stream.id).ended_by, LiveEnd::Timeout);
    EXPECT_EQ(row(running->stream.id).state, LiveState::Live);
    clock.advance(core::Millis{3'000'000});
    sweep();
    EXPECT_EQ(row(running->stream.id).ended_by, LiveEnd::Timeout);
    EXPECT_EQ(live->counters().ended.at(static_cast<std::size_t>(LiveEnd::Timeout)), 2U);
}

TEST_F(LiveStreamsTest, ASweepThatCannotReadOrAskGoesOnNextTime) {
    const auto started = create("alice");
    ASSERT_TRUE(started);
    ASSERT_TRUE(go_live(started->stream.id, "alice"));
    store->fail = LiveStoreError::Unavailable;
    sweep();
    store->fail.reset();
    packagers->fail_state = PackagerError::Unavailable;
    sweep();
    EXPECT_EQ(row(started->stream.id).state, LiveState::Live);
    packagers->fail_state.reset();
    packagers->states[started->stream.id.to_string()] = PackagerState::Finished;
    sweep();
    EXPECT_EQ(row(started->stream.id).state, LiveState::Ended);
}

TEST_F(LiveStreamsTest, SweepsRepeatOnTheirInterval) {
    LiveSettings settings;
    settings.sweep_interval = core::Millis{10'000};
    make(settings);
    live->start_sweeping();
    live->start_sweeping();
    ulw::test::pump_pending(*reactor);
    EXPECT_EQ(live->counters().sweeps, 0U);
    clock.advance(settings.sweep_interval);
    ASSERT_TRUE(pump_until(*reactor, [&] { return live->counters().sweeps == 1; }));
    ulw::test::pump_pending(*reactor);
    clock.advance(settings.sweep_interval);
    ASSERT_TRUE(pump_until(*reactor, [&] { return live->counters().sweeps == 2; }));
}

TEST(LiveFailureNames, EveryFailureHasOne) {
    for (const LiveFailure f : {LiveFailure::NotFound, LiveFailure::Ended, LiveFailure::Full,
                                LiveFailure::Unavailable, LiveFailure::Internal}) {
        EXPECT_FALSE(gateway::to_string(f).empty());
    }
}

} // namespace
