// LiveKit's webhooks as LiveKit itself sends them (ADR-0093): the LiveKit of
// deploy/local/compose.yaml (the calls profile) posts its room events, signed with its key pair,
// to 127.0.0.1:7890 (ULW_LIVE_WEBHOOK_TEST_PORT elsewhere), where this test listens with the
// gateway's webhook server, publisher watch and stream service over a scratch Postgres. A live
// stream whose room LiveKit then finishes ends `publisher_left` once LiveKit, asked, confirms
// that its publisher is gone. Skips when LiveKit is not running or the port is taken.
#include "core/util/parse.hpp"
#include "infra/curl/http.hpp"
#include "infra/curl/multi.hpp"
#include "infra/postgres/live_streams.hpp"
#include "infra/sfu/livekit/livekit_sfu.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"
#include "net/socket.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "../gateway/live_fakes.hpp"
#include "../gateway/webhook_signer.hpp"
#include "live_streams.hpp"
#include "ops/log.hpp"
#include "postgres_harness.hpp"
#include "publisher_watch.hpp"
#include "support/memory_log.hpp"
#include "support/reactor_harness.hpp"
#include "webhook_server.hpp"

#include <sys/socket.h>

#include <cstdlib>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>

namespace {

using core::ports::LiveEnd;
using core::ports::LiveState;
using gateway::LiveFailure;
using ulw::test::ScratchDatabase;

std::string env_or(const char* name, const char* fallback) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread of the test exists.
    const char* value = std::getenv(name);
    return value != nullptr ? value : fallback;
}

const std::string kApiUrl = env_or("LIVEKIT_API_URL", "http://127.0.0.1:7880");
const std::string kKey = env_or("LIVEKIT_API_KEY", "ulw-dev-key");
const std::string kSecret =
    env_or("LIVEKIT_API_SECRET", "ulw-dev-secret-testtest123-not-a-real-secret");
const std::string kHookPort = env_or("ULW_LIVE_WEBHOOK_TEST_PORT", "7890");

class LiveWebhookTest : public ::testing::Test {
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
        const auto port = core::parse_integer<std::uint16_t>(kHookPort);
        ASSERT_TRUE(port);
        auto listener = net::listen_tcp({.port = *port, .loopback_only = true});
        if (!listener) {
            GTEST_SKIP() << "port " << *port << " is taken: another gateway receives the webhooks";
        }
        hook_port = *port;
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
             .api_key = kKey,
             .api_secret = kSecret,
             .packager_srt = "srt://127.0.0.1:9"});
        ASSERT_TRUE(sfu_made);
        sfu = std::move(*sfu_made);
        packagers = std::make_unique<ulw::test::FakePackagers>(*reactor);
        live = std::make_unique<gateway::LiveStreams>(gateway::LiveDeps{.reactor = *reactor,
                                                                        .store = *store,
                                                                        .sfu = *sfu,
                                                                        .packagers = *packagers,
                                                                        .clock = clock,
                                                                        .random = random,
                                                                        .log = log},
                                                      gateway::LiveSettings{});
        watch = std::make_unique<gateway::PublisherWatch>(
            *reactor, *live, log, gateway::WatchSettings{.grace = core::Millis{300}});
        server = std::make_unique<gateway::WebhookServer>(
            *reactor, clock, gateway::WebhookKey{.id = kKey, .secret = kSecret}, *watch, log,
            gateway::WebhookLimits{});
        ASSERT_TRUE(reactor->listen(std::move(*listener), *server));
    }

    void TearDown() override {
        server.reset();
        watch.reset();
        offload.reset();
        live.reset();
        packagers.reset();
        sfu.reset();
        store.reset();
        multi.reset();
        reactor.reset();
        db.reset();
    }

    bool pump(const std::function<bool()>& until, std::chrono::seconds limit) {
        return ulw::test::pump_until(
            *reactor,
            [&] {
                server->reap();
                return until();
            },
            limit);
    }

    std::optional<core::ports::LiveStream> status(const core::LiveStreamId& id) {
        std::optional<std::expected<core::ports::LiveStream, LiveFailure>> seen;
        live->status(id, [&](auto r) noexcept { seen = std::move(r); });
        if (!pump([&] { return seen.has_value(); }, std::chrono::seconds(10)) || !*seen) {
            return std::nullopt;
        }
        return **seen;
    }

    os::SystemClock clock;
    os::SystemRandom random;
    ulw::test::MemoryLog sink;
    ops::Logger log{sink, clock, "gateway", ops::Level::Debug};
    std::uint16_t hook_port = 0;
    std::unique_ptr<ScratchDatabase> db;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<net::OffloadPool> offload;
    std::unique_ptr<infra::curl::Multi> multi;
    std::unique_ptr<infra::postgres::PgLiveStreams> store;
    std::unique_ptr<core::ports::ISfu> sfu;
    std::unique_ptr<ulw::test::FakePackagers> packagers;
    std::unique_ptr<gateway::LiveStreams> live;
    std::unique_ptr<gateway::PublisherWatch> watch;
    std::unique_ptr<gateway::WebhookServer> server;
};

TEST_F(LiveWebhookTest, ALiveStreamWhoseRoomLiveKitFinishedEndsPublisherLeft) {
    // A stream, its room opened by its first ticket, and live as if its owner had gone live (no
    // publisher or recorder here: the browser suite brings those).
    std::optional<std::expected<gateway::StartedStream, LiveFailure>> created;
    live->create(*core::UserId::parse("auth0|hooked"),
                 [&](auto r) noexcept { created = std::move(r); });
    ASSERT_TRUE(pump([&] { return created.has_value(); }, std::chrono::seconds(20)));
    ASSERT_TRUE(*created) << gateway::to_string(created->error());
    const core::LiveStreamId id = (*created)->stream.id;
    std::optional<core::ports::LiveResult<core::ports::LiveStream>> marked;
    store->mark_live(id, clock.wall_now(), [&](auto r) noexcept { marked = std::move(r); });
    ASSERT_TRUE(pump([&] { return marked.has_value(); }, std::chrono::seconds(10)));
    ASSERT_TRUE(*marked);

    // LiveKit tells of the room it made (room_started), signed as LiveKit signs.
    ASSERT_TRUE(pump([&] { return server->counters().accepted >= 1; }, std::chrono::seconds(20)))
        << "no webhook from LiveKit: is its configuration's webhook block posting to 127.0.0.1:"
        << hook_port << "?";

    // The room goes, as when everyone has left it and its timeout ran out.
    std::optional<std::expected<void, core::ports::MediaError>> closed;
    sfu->open_room(*core::RoomId::parse(id.to_string()), core::ports::MediaGeneration{1},
                   core::ports::MediaRoomKind::Stream, 0, [&](auto opened) noexcept {
                       ASSERT_TRUE(opened);
                       core::ports::IMediaRoom* room = opened->get();
                       room->close([&, held = std::move(*opened)](auto r) mutable noexcept {
                           held.reset();
                           closed = r;
                       });
                   });
    ASSERT_TRUE(pump([&] { return closed.has_value(); }, std::chrono::seconds(10)));
    ASSERT_TRUE(*closed);

    // room_finished, the grace, LiveKit's word that nobody is there, and the end.
    ASSERT_TRUE(pump([&] { return watch->counters().ended == 1U; }, std::chrono::seconds(30)))
        << sink.all();
    const auto ended = status(id);
    ASSERT_TRUE(ended);
    EXPECT_EQ(ended->state, LiveState::Ended);
    EXPECT_EQ(ended->ended_by, LiveEnd::PublisherLeft);
    EXPECT_GE(watch->counters().departures, 1U);
    for (const std::uint64_t n : server->counters().refused) {
        EXPECT_EQ(n, 0U) << "LiveKit's own webhook refused";
    }
    EXPECT_EQ(server->counters().bad_requests, 0U);
}

TEST_F(LiveWebhookTest, ForgedWebhooksAreRefusedOnTheSamePort) {
    const std::string body = R"({"event":"room_finished","room":{"name":"x:1"}})";
    const std::string forged = ulw::test::sign_webhook(
        body,
        {.key = kKey,
         .secret = "fake-not-livekits-secret-0123456789abcdef",
         .issued = 0,
         .expires =
             std::chrono::floor<std::chrono::seconds>(clock.wall_now().time_since_epoch()).count() +
             300});
    const std::string request =
        "POST /livekit/webhook HTTP/1.1\r\nHost: x\r\nAuthorization: " + forged +
        "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    auto fd = ulw::test::connect_loopback(hook_port);
    ASSERT_TRUE(fd);
    ASSERT_EQ(ulw::test::write_some(fd.get(), std::as_bytes(std::span(request))), request.size());
    std::string answer;
    ASSERT_TRUE(pump(
        [&] {
            std::array<char, 1024> buf{};
            const ssize_t n = ::recv(fd.get(), buf.data(), buf.size(), MSG_DONTWAIT);
            if (n > 0) {
                answer.append(buf.data(), static_cast<std::size_t>(n));
            }
            return answer.contains("\r\n\r\n");
        },
        std::chrono::seconds(10)));
    EXPECT_TRUE(answer.starts_with("HTTP/1.1 401 ")) << answer;
    EXPECT_EQ(server->counters().refused.at(
                  static_cast<std::size_t>(gateway::WebhookRejection::Signature)),
              1U);
}

} // namespace
