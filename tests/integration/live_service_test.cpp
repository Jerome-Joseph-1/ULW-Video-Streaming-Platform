// The stream service against what it runs on in production but the packagers: the Postgres and
// the LiveKit of deploy/local/compose.yaml (the calls profile). Going live needs a publisher in
// the room and LiveKit's recorder, which the browser suite brings (tests/e2e/live-publish).
#include "core/util/json.hpp"
#include "infra/curl/http.hpp"
#include "infra/curl/multi.hpp"
#include "infra/postgres/live_streams.hpp"
#include "infra/sfu/livekit/livekit_sfu.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "../gateway/live_fakes.hpp"
#include "../unit/sfu/token_reader.hpp"
#include "live_streams.hpp"
#include "ops/log.hpp"
#include "postgres_harness.hpp"
#include "support/memory_log.hpp"
#include "support/reactor_harness.hpp"

#include <cstdlib>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>

namespace {

using core::ports::LiveEnd;
using core::ports::LiveState;
using gateway::LiveFailure;
using gateway::LiveStreams;
using ulw::test::ScratchDatabase;

std::string env_or(const char* name, const char* fallback) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread of the test exists.
    const char* value = std::getenv(name);
    return value != nullptr ? value : fallback;
}

const std::string kApiUrl = env_or("LIVEKIT_API_URL", "http://127.0.0.1:7880");
const std::string kSecret =
    env_or("LIVEKIT_API_SECRET", "ulw-dev-secret-testtest123-not-a-real-secret");

template <class T> using Result = std::optional<std::expected<T, LiveFailure>>;

class LiveServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto up = infra::curl::perform({.method = infra::curl::Method::Get,
                                              .url = kApiUrl + "/",
                                              .headers = {},
                                              .max_body = 1024,
                                              .timeout = std::chrono::seconds(2)});
        if (!up) {
            GTEST_SKIP() << "LiveKit unreachable at " << kApiUrl
                         << "; start deploy/local/compose.yaml --profile calls";
        }
        ScratchDatabase::open(db);
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        reactor = std::move(*net::make_reactor(net::ReactorKind::Epoll, clock, 256));
        offload = std::move(*net::OffloadPool::create(*reactor, 1));
        multi = std::move(*infra::curl::Multi::create(*reactor));
        auto made = infra::postgres::PgLiveStreams::create(*reactor, *offload,
                                                           {.conninfo = db->conninfo()});
        ASSERT_TRUE(made) << made.error();
        store = std::move(*made);
        auto sfu_made = infra::sfu::livekit::make_sfu(
            *reactor, *multi, clock,
            {.api_url = kApiUrl,
             .client_url = env_or("LIVEKIT_CLIENT_URL", "ws://127.0.0.1:7880"),
             .api_key = env_or("LIVEKIT_API_KEY", "ulw-dev-key"),
             .api_secret = kSecret,
             .packager_srt = "srt://127.0.0.1:9"});
        ASSERT_TRUE(sfu_made);
        sfu = std::move(*sfu_made);
        packagers = std::make_unique<ulw::test::FakePackagers>(*reactor);
        live = std::make_unique<LiveStreams>(gateway::LiveDeps{.reactor = *reactor,
                                                               .store = *store,
                                                               .sfu = *sfu,
                                                               .packagers = *packagers,
                                                               .clock = clock,
                                                               .random = random,
                                                               .log = log},
                                             gateway::LiveSettings{});
    }

    void TearDown() override {
        offload.reset();
        live.reset();
        packagers.reset();
        sfu.reset();
        store.reset();
        multi.reset();
        reactor.reset();
        db.reset();
    }

    template <class T> std::expected<T, LiveFailure> wait(Result<T>& r) {
        EXPECT_TRUE(ulw::test::pump_until(
            *reactor, [&] { return r.has_value(); }, std::chrono::seconds(20)));
        return r ? std::move(*r) : std::unexpected(LiveFailure::Internal);
    }

    os::SystemClock clock;
    os::SystemRandom random;
    ulw::test::MemoryLog sink;
    ops::Logger log{sink, clock, "gateway", ops::Level::Debug};
    std::unique_ptr<ScratchDatabase> db;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<net::OffloadPool> offload;
    std::unique_ptr<infra::curl::Multi> multi;
    std::unique_ptr<infra::postgres::PgLiveStreams> store;
    std::unique_ptr<core::ports::ISfu> sfu;
    std::unique_ptr<ulw::test::FakePackagers> packagers;
    std::unique_ptr<LiveStreams> live;
};

TEST_F(LiveServiceTest, AStreamsTicketsPublishIntoItsOwnRoomAsItsOneIdentityUntilItEnds) {
    const auto owner = *core::UserId::parse("auth0|caster");
    Result<gateway::StartedStream> created;
    live->create(owner, [&](auto r) noexcept { created = std::move(r); });
    const auto started = wait(created);
    ASSERT_TRUE(started) << gateway::to_string(started.error());
    const std::string id = started->stream.id.to_string();
    EXPECT_EQ(started->ticket.endpoint, "http://127.0.0.1:7880/whip/v1");
    const auto token = ulw::test::read_token(started->ticket.credential, kSecret);
    ASSERT_TRUE(token);
    EXPECT_EQ(ulw::test::string_at(token->claims, "video", "room"), id + ":1");
    EXPECT_EQ(ulw::test::string_at(token->claims, "sub"), "auth0|caster/" + id);
    EXPECT_EQ(ulw::test::bool_at(token->claims, "video", "canPublish"), true);
    EXPECT_EQ(ulw::test::bool_at(token->claims, "video", "canSubscribe"), false);

    // Someone else gets nothing; the owner a fresh ticket for the same identity.
    Result<core::ports::MediaTicket> other;
    live->ticket(started->stream.id, *core::UserId::parse("auth0|other"),
                 [&](auto r) noexcept { other = std::move(r); });
    EXPECT_EQ(wait(other), std::unexpected(LiveFailure::NotFound));
    Result<core::ports::MediaTicket> fresh;
    live->ticket(started->stream.id, owner, [&](auto r) noexcept { fresh = std::move(r); });
    const auto again = wait(fresh);
    ASSERT_TRUE(again);
    EXPECT_EQ(
        ulw::test::string_at(ulw::test::read_token(again->credential, kSecret)->claims, "sub"),
        "auth0|caster/" + id);

    // The end closes the room at LiveKit and the stream to tickets.
    Result<core::ports::LiveStream> ended;
    live->end(started->stream.id, owner, [&](auto r) noexcept { ended = std::move(r); });
    const auto end = wait(ended);
    ASSERT_TRUE(end) << gateway::to_string(end.error());
    EXPECT_EQ(end->state, LiveState::Ended);
    EXPECT_EQ(end->ended_by, LiveEnd::Owner);
    Result<core::ports::MediaTicket> late;
    live->ticket(started->stream.id, owner, [&](auto r) noexcept { late = std::move(r); });
    EXPECT_EQ(wait(late), std::unexpected(LiveFailure::Ended));
}

TEST_F(LiveServiceTest, TheSweepEndsAStreamNobodyTookLive) {
    gateway::LiveSettings settings;
    settings.start_window = core::Seconds{0};
    live = std::make_unique<LiveStreams>(gateway::LiveDeps{.reactor = *reactor,
                                                           .store = *store,
                                                           .sfu = *sfu,
                                                           .packagers = *packagers,
                                                           .clock = clock,
                                                           .random = random,
                                                           .log = log},
                                         settings);
    Result<gateway::StartedStream> created;
    live->create(*core::UserId::parse("auth0|idle"),
                 [&](auto r) noexcept { created = std::move(r); });
    const auto started = wait(created);
    ASSERT_TRUE(started);
    // A window of nothing: any time after the creation is past it.
    ASSERT_TRUE(ulw::test::pump_until(
        *reactor, [&] { return clock.wall_now() > started->stream.created_at; }));
    live->sweep_now();
    ASSERT_TRUE(ulw::test::pump_until(
        *reactor, [&] { return !live->sweeping(); }, std::chrono::seconds(20)));
    Result<core::ports::LiveStream> seen;
    live->status(started->stream.id, [&](auto r) noexcept { seen = std::move(r); });
    const auto status = wait(seen);
    ASSERT_TRUE(status);
    EXPECT_EQ(status->state, LiveState::Ended);
    EXPECT_EQ(status->ended_by, LiveEnd::Timeout);
    EXPECT_EQ(live->counters().media_failures, 0U);
}

} // namespace
