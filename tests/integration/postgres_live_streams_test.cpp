// PgLiveStreams against the Postgres of deploy/local/compose.yaml, on a scratch database
// migrated to the bundled schema (migration 0011).
#include "core/ports/live.hpp"
#include "infra/postgres/live_streams.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "postgres_harness.hpp"
#include "support/reactor_harness.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using core::ports::CreatedLiveStream;
using core::ports::EndedLiveStream;
using core::ports::LiveEnd;
using core::ports::LiveResult;
using core::ports::LiveState;
using core::ports::LiveStoreError;
using core::ports::LiveStream;
using infra::postgres::LiveStreamsConfig;
using infra::postgres::Params;
using infra::postgres::PgLiveStreams;
using ulw::test::scalar;
using ulw::test::ScratchDatabase;

constexpr std::string_view kPassphrase = "fake-srt-passphrase-testtest123";

core::UserId user(std::string_view name) {
    return *core::UserId::parse(name);
}

// Whole microseconds, as the database keeps them.
core::WallTime at_seconds(std::int64_t s) {
    return core::WallTime{std::chrono::seconds{s}};
}

class PgLiveStreamsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ScratchDatabase::open(db);
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        auto r = net::make_reactor(net::ReactorKind::Epoll, clock, 256);
        ASSERT_TRUE(r) << std::strerror(r.error());
        reactor = std::move(*r);
        auto p = net::OffloadPool::create(*reactor, 1);
        ASSERT_TRUE(p);
        offload = std::move(*p);
        auto made = PgLiveStreams::create(*reactor, *offload,
                                          LiveStreamsConfig{.conninfo = db->conninfo()});
        ASSERT_TRUE(made) << made.error();
        store = std::move(*made);
    }

    void TearDown() override {
        offload.reset();
        store.reset();
        reactor.reset();
        db.reset();
    }

    template <class T> LiveResult<T> wait(std::optional<LiveResult<T>>& r) {
        if (!ulw::test::pump_until(
                *reactor, [&] { return r.has_value(); }, std::chrono::seconds(15))) {
            ADD_FAILURE() << "never answered";
            return std::unexpected(LiveStoreError::Unavailable);
        }
        return std::move(*r);
    }

    LiveResult<CreatedLiveStream> create(std::string_view owner, std::int64_t max = 10,
                                         std::int64_t at = 1'767'225'600,
                                         std::int64_t per_hour = 100) {
        std::optional<LiveResult<CreatedLiveStream>> r;
        store->create({.id = core::LiveStreamId::generate(clock, random),
                       .owner = user(owner),
                       .passphrase = std::string(kPassphrase),
                       .at = at_seconds(at)},
                      {.max_unfinished = max, .per_owner_per_hour = per_hour},
                      [&](auto x) noexcept { r = std::move(x); });
        return wait(r);
    }

    LiveResult<LiveStream> find(const core::LiveStreamId& id) {
        std::optional<LiveResult<LiveStream>> r;
        store->find(id, [&](auto x) noexcept { r = std::move(x); });
        return wait(r);
    }

    LiveResult<LiveStream> mark_live(const core::LiveStreamId& id, std::int64_t at) {
        std::optional<LiveResult<LiveStream>> r;
        store->mark_live(id, at_seconds(at), [&](auto x) noexcept { r = std::move(x); });
        return wait(r);
    }

    LiveResult<EndedLiveStream> end(const core::LiveStreamId& id, LiveEnd reason, std::int64_t at) {
        std::optional<LiveResult<EndedLiveStream>> r;
        store->end(id, reason, at_seconds(at), [&](auto x) noexcept { r = std::move(x); });
        return wait(r);
    }

    LiveResult<std::vector<LiveStream>> unfinished(std::size_t limit) {
        std::optional<LiveResult<std::vector<LiveStream>>> r;
        store->unfinished(limit, [&](auto x) noexcept { r = std::move(x); });
        return wait(r);
    }

    os::SystemClock clock;
    os::SystemRandom random;
    std::unique_ptr<ScratchDatabase> db;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<net::OffloadPool> offload;
    std::unique_ptr<PgLiveStreams> store;
};

TEST_F(PgLiveStreamsTest, AStreamIsStoredStartingAndItsLiveChatIsOpened) {
    const auto made = create("auth0|alice");
    ASSERT_TRUE(made) << core::ports::to_string(made.error());
    EXPECT_TRUE(made->created);
    const LiveStream& s = made->stream;
    EXPECT_EQ(s.owner, user("auth0|alice"));
    EXPECT_EQ(s.state, LiveState::Starting);
    EXPECT_EQ(s.passphrase, kPassphrase);
    EXPECT_EQ(s.created_at, at_seconds(1'767'225'600));
    EXPECT_FALSE(s.live_at || s.ended_at || s.ended_by || s.recording);
    const auto found = find(s.id);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->id, s.id);
    // The stream's chat is open to viewers by the stream's name (ADR-0070).
    auto conn = db->session();
    EXPECT_EQ(scalar(conn,
                     "SELECT kind FROM chat_rooms WHERE room_id = live_chat_room($1::uuid::text)",
                     Params{}.add_uuid(s.id.uuid())),
              "stream_live_chat");
}

TEST_F(PgLiveStreamsTest, AnOwnerHasOneUnfinishedStreamAndThePlatformAFewAtMost) {
    const auto first = create("alice");
    ASSERT_TRUE(first);
    const auto again = create("alice");
    ASSERT_TRUE(again);
    EXPECT_FALSE(again->created);
    EXPECT_EQ(again->stream.id, first->stream.id);
    // Two unfinished of at most two: bob's makes the second, carol's finds no room.
    ASSERT_TRUE(create("bob", 2));
    EXPECT_EQ(create("carol", 2), std::unexpected(LiveStoreError::Full));
    // An owner already running one is answered it, however full the platform is.
    const auto alice = create("alice", 2);
    ASSERT_TRUE(alice);
    EXPECT_FALSE(alice->created);
    ASSERT_TRUE(end(first->stream.id, LiveEnd::Owner, 1'767'225'700));
    const auto next = create("alice", 2);
    ASSERT_TRUE(next);
    EXPECT_TRUE(next->created);
    EXPECT_NE(next->stream.id, first->stream.id);
}

TEST_F(PgLiveStreamsTest, AnOwnerCreatesOnlySoManyStreamsAnHour) {
    for (int i = 0; i < 3; ++i) {
        const auto made = create("alice", 10, 1'767'225'600 + (i * 60), 3);
        ASSERT_TRUE(made);
        ASSERT_TRUE(end(made->stream.id, LiveEnd::Owner, 1'767'225'610 + (i * 60)));
    }
    EXPECT_EQ(create("alice", 10, 1'767'225'800, 3), std::unexpected(LiveStoreError::TooMany));
    // Others are not held to alice's count, and alice is free again an hour after her first.
    EXPECT_TRUE(create("bob", 10, 1'767'225'800, 3));
    EXPECT_TRUE(create("alice", 10, 1'767'229'201, 3));
}

TEST_F(PgLiveStreamsTest, ConcurrentCreatesNeverPassTheCapTogether) {
    // Six owners at once, against a cap of two, on four sessions.
    std::vector<std::optional<LiveResult<CreatedLiveStream>>> answers(6);
    for (std::size_t i = 0; i < answers.size(); ++i) {
        store->create({.id = core::LiveStreamId::generate(clock, random),
                       .owner = user("racer-" + std::to_string(i)),
                       .passphrase = std::string(kPassphrase),
                       .at = at_seconds(1'767'225'600)},
                      {.max_unfinished = 2, .per_owner_per_hour = 100},
                      [&answers, i](auto x) noexcept { answers[i] = std::move(x); });
    }
    ASSERT_TRUE(ulw::test::pump_until(*reactor, [&] {
        return std::ranges::all_of(answers, [](const auto& a) { return a.has_value(); });
    }));
    std::size_t created = 0;
    std::size_t full = 0;
    for (const auto& a : answers) {
        created += *a ? 1U : 0U;
        full += !*a && a->error() == LiveStoreError::Full ? 1U : 0U;
    }
    EXPECT_EQ(created, 2U);
    EXPECT_EQ(full, 4U);
}

TEST_F(PgLiveStreamsTest, AStreamsEndClosesItsLiveChat) {
    const auto made = create("alice");
    ASSERT_TRUE(made);
    auto conn = db->session();
    const auto closed = [&] {
        return scalar(conn,
                      "SELECT closed_at IS NOT NULL FROM chat_rooms "
                      "WHERE room_id = live_chat_room($1::uuid::text)",
                      Params{}.add_uuid(made->stream.id.uuid()));
    };
    EXPECT_EQ(closed(), "f");
    ASSERT_TRUE(end(made->stream.id, LiveEnd::Owner, 1'767'225'700));
    EXPECT_EQ(closed(), "t");
    // The chat store reads it as closed now: a viewer's join is refused.
    EXPECT_EQ(scalar(conn,
                     "SELECT CASE WHEN closed_at IS NULL THEN kind ELSE 'group_chat' END "
                     "FROM chat_rooms WHERE room_id = live_chat_room($1::uuid::text)",
                     Params{}.add_uuid(made->stream.id.uuid())),
              "group_chat");
}

TEST_F(PgLiveStreamsTest, AStreamGoesLiveOnceAndEndsOnce) {
    const auto made = create("alice");
    ASSERT_TRUE(made);
    const core::LiveStreamId id = made->stream.id;
    const auto live = mark_live(id, 1'767'225'610);
    ASSERT_TRUE(live);
    EXPECT_EQ(live->state, LiveState::Live);
    EXPECT_EQ(live->live_at, at_seconds(1'767'225'610));
    // Marking again keeps the first time.
    EXPECT_EQ(mark_live(id, 1'767'225'620)->live_at, at_seconds(1'767'225'610));

    const auto ended = end(id, LiveEnd::Finished, 1'767'225'700);
    ASSERT_TRUE(ended);
    EXPECT_TRUE(ended->ended);
    EXPECT_EQ(ended->stream.state, LiveState::Ended);
    EXPECT_EQ(ended->stream.ended_by, LiveEnd::Finished);
    EXPECT_EQ(ended->stream.ended_at, at_seconds(1'767'225'700));
    EXPECT_EQ(ended->stream.live_at, at_seconds(1'767'225'610));
    // A second end, for another reason, finds the first standing.
    const auto again = end(id, LiveEnd::Owner, 1'767'225'800);
    ASSERT_TRUE(again);
    EXPECT_FALSE(again->ended);
    EXPECT_EQ(again->stream.ended_by, LiveEnd::Finished);
    EXPECT_EQ(again->stream.ended_at, at_seconds(1'767'225'700));
    // An ended stream is never live again.
    EXPECT_EQ(mark_live(id, 1'767'225'900)->state, LiveState::Ended);
}

TEST_F(PgLiveStreamsTest, APublisherWhoLeftIsAReasonTheRowKeeps) {
    const auto made = create("auth0|left");
    ASSERT_TRUE(made);
    const auto ended = end(made->stream.id, LiveEnd::PublisherLeft, 1'767'226'200);
    ASSERT_TRUE(ended);
    EXPECT_TRUE(ended->ended);
    EXPECT_EQ(ended->stream.ended_by, LiveEnd::PublisherLeft);
    const auto again = end(made->stream.id, LiveEnd::Owner, 1'767'226'300);
    ASSERT_TRUE(again);
    EXPECT_EQ(again->stream.ended_by, LiveEnd::PublisherLeft);
}

TEST_F(PgLiveStreamsTest, AStreamNeverLiveEndsWithoutALiveTime) {
    const auto made = create("alice");
    ASSERT_TRUE(made);
    const auto ended = end(made->stream.id, LiveEnd::Timeout, 1'767'226'200);
    ASSERT_TRUE(ended);
    EXPECT_EQ(ended->stream.ended_by, LiveEnd::Timeout);
    EXPECT_FALSE(ended->stream.live_at);
}

TEST_F(PgLiveStreamsTest, UnknownStreamsAreNotFound) {
    const auto id = core::LiveStreamId::generate(clock, random);
    EXPECT_EQ(find(id), std::unexpected(LiveStoreError::NotFound));
    EXPECT_EQ(mark_live(id, 1), std::unexpected(LiveStoreError::NotFound));
    EXPECT_EQ(end(id, LiveEnd::Owner, 1), std::unexpected(LiveStoreError::NotFound));
}

TEST_F(PgLiveStreamsTest, TheRecordingIsTheVideoThePackagerQueued) {
    const auto made = create("alice");
    ASSERT_TRUE(made);
    ASSERT_TRUE(end(made->stream.id, LiveEnd::Finished, 1'767'225'700));
    const auto video = core::VideoId::generate(clock, random);
    auto conn = db->session();
    ASSERT_TRUE(conn.exec("INSERT INTO live_recordings (stream_id, video_id) VALUES ($1, $2)",
                          Params{}.add_text(made->stream.id.to_string()).add_uuid(video.uuid())));
    const auto found = find(made->stream.id);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->recording, video);
}

TEST_F(PgLiveStreamsTest, TheUnfinishedAreListedOldestFirst) {
    const auto a = create("a", 10, 1'767'225'603);
    const auto b = create("b", 10, 1'767'225'601);
    const auto c = create("c", 10, 1'767'225'602);
    ASSERT_TRUE(a && b && c);
    ASSERT_TRUE(end(c->stream.id, LiveEnd::Owner, 1'767'225'700));
    const auto listed = unfinished(10);
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed->size(), 2U);
    EXPECT_EQ((*listed)[0].id, b->stream.id);
    EXPECT_EQ((*listed)[1].id, a->stream.id);
    const auto one = unfinished(1);
    ASSERT_TRUE(one);
    EXPECT_EQ(one->size(), 1U);
    EXPECT_EQ(unfinished(0), std::unexpected(LiveStoreError::NotFound));
}

TEST_F(PgLiveStreamsTest, TheSchemaRefusesARowThatBreaksTheRules) {
    auto conn = db->session();
    // An ended stream without a reason, and a live one without a time.
    EXPECT_FALSE(conn.exec("INSERT INTO live_streams (id, owner_id, state, srt_passphrase, "
                           "created_at, ended_at) VALUES (gen_random_uuid(), 'x', 'ended', "
                           "'0123456789ab', now(), now())"));
    EXPECT_FALSE(conn.exec("INSERT INTO live_streams (id, owner_id, state, srt_passphrase, "
                           "created_at) VALUES (gen_random_uuid(), 'x', 'live', '0123456789ab', "
                           "now())"));
    EXPECT_FALSE(conn.exec("INSERT INTO live_streams (id, owner_id, srt_passphrase, created_at) "
                           "VALUES (gen_random_uuid(), 'x', 'short', now())"));
}

TEST_F(PgLiveStreamsTest, ACorruptRowIsReportedAsSuch) {
    auto conn = db->session();
    const auto id = core::LiveStreamId::generate(clock, random);
    // An owner id the domain refuses.
    ASSERT_TRUE(conn.exec("INSERT INTO live_streams (id, owner_id, srt_passphrase, created_at) "
                          "VALUES ($1, 'not an id', '0123456789ab', now())",
                          Params{}.add_uuid(id.uuid())));
    EXPECT_EQ(find(id), std::unexpected(LiveStoreError::Corrupt));
    EXPECT_EQ(unfinished(5), std::unexpected(LiveStoreError::Corrupt));
}

} // namespace
