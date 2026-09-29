#include "core/models/ids.hpp"
#include "core/ports/media.hpp"
#include "core/util/json.hpp"
#include "infra/sfu/livekit/livekit_sfu.hpp"
#include "net/reactor_factory.hpp"
#include "net/socket.hpp"
#include "os/system_clock.hpp"

#include "support/fake_clock.hpp"
#include "support/http_test_server.hpp"
#include "support/reactor_harness.hpp"
#include "token_reader.hpp"

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

using core::ports::IMediaRoom;
using core::ports::ISfu;
using core::ports::MediaError;
using core::ports::MediaGeneration;
using infra::sfu::livekit::Config;
using infra::sfu::livekit::ConfigError;
using infra::sfu::livekit::make_sfu;
using ulw::test::bool_at;
using ulw::test::HttpTestServer;
using ulw::test::pump_until;
using ulw::test::read_token;
using ulw::test::Reply;
using ulw::test::ServedRequest;
using ulw::test::string_at;

constexpr std::string_view kSecret = "fake-secret-for-the-adapter-tests-0123456789";
constexpr std::string_view kRoom = "0192f3a4-0000-7000-8000-000000000001";
constexpr std::string_view kDevice = "0192f3a4-0000-7000-8000-00000000000d";

using OpenResult = std::expected<std::unique_ptr<IMediaRoom>, MediaError>;
using DoneResult = std::expected<void, MediaError>;
using TicketResult = std::expected<core::ports::MediaTicket, MediaError>;

Config config_for(std::string api_url) {
    return Config{.api_url = std::move(api_url),
                  .client_url = "wss://media.example.test",
                  .api_key = "fake-key",
                  .api_secret = std::string(kSecret)};
}

class LiveKitSfuTest : public ::testing::TestWithParam<net::ReactorKind> {
protected:
    void SetUp() override {
        auto r = net::make_reactor(GetParam(), reactor_clock, 1024);
        ASSERT_TRUE(r);
        reactor = std::move(*r);
        auto m = infra::curl::Multi::create(*reactor);
        ASSERT_TRUE(m);
        multi = std::move(*m);
    }
    void TearDown() override {
        sfu.reset();
        multi.reset();
        reactor.reset();
    }

    void start(std::string api_url) {
        auto made = make_sfu(*reactor, *multi, clock, config_for(std::move(api_url)));
        ASSERT_TRUE(made);
        sfu = std::move(*made);
    }

    OpenResult open(MediaGeneration generation = MediaGeneration{1}) {
        std::optional<OpenResult> got;
        sfu->open_room(*core::RoomId::parse(kRoom), generation, 2,
                       [&](OpenResult r) noexcept { got = std::move(r); });
        EXPECT_FALSE(got.has_value()) << "callback ran inside open_room()";
        EXPECT_TRUE(pump_until(*reactor, [&] { return got.has_value(); }));
        return std::move(*got);
    }

    TicketResult join(IMediaRoom& room, std::string_view user, std::string_view device = kDevice) {
        std::optional<TicketResult> got;
        room.join(*core::UserId::parse(user), *core::DeviceId::parse(device),
                  [&](TicketResult r) noexcept { got = std::move(r); });
        EXPECT_FALSE(got.has_value()) << "callback ran inside join()";
        EXPECT_TRUE(pump_until(*reactor, [&] { return got.has_value(); }));
        return std::move(*got);
    }

    template <class Start> DoneResult wait(Start start) {
        std::optional<DoneResult> got;
        start([&](DoneResult r) noexcept { got = r; });
        EXPECT_FALSE(got.has_value()) << "callback ran inside the call";
        EXPECT_TRUE(pump_until(*reactor, [&] { return got.has_value(); }));
        return *got;
    }

    os::SystemClock reactor_clock;
    ulw::test::FakeClock clock;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<infra::curl::Multi> multi;
    std::unique_ptr<ISfu> sfu;
};

HttpTestServer answering(int status) {
    return HttpTestServer([status](const ServedRequest&) {
        return Reply{.status = status,
                     .headers = {{"Content-Type", "application/json"}},
                     .body = status == 200 ? "{}" : R"({"code":"x","msg":"y"})"};
    });
}

core::json::Value body_of(const ServedRequest& request) {
    auto body = core::json::parse(request.body);
    EXPECT_TRUE(body) << request.body;
    return body ? std::move(*body) : core::json::Value{};
}

// The bearer token of a served request, its signature checked with the configured secret.
core::json::Value claims_of(const ServedRequest& request) {
    const auto auth = request.header("authorization");
    EXPECT_TRUE(auth && auth->starts_with("Bearer "));
    auto token = read_token(auth->substr(7), kSecret);
    EXPECT_TRUE(token) << "bad signature";
    return token ? std::move(token->claims) : core::json::Value{};
}

TEST_P(LiveKitSfuTest, OpeningARoomCreatesItsGenerationWithTheCallsLimits) {
    auto server = answering(200);
    start(server.base_url() + "/");
    ASSERT_TRUE(open(MediaGeneration{7}));

    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 1U);
    const ServedRequest& create = requests[0];
    EXPECT_EQ(create.method, "POST");
    EXPECT_EQ(create.path(), "/twirp/livekit.RoomService/CreateRoom");
    EXPECT_EQ(create.header("content-type"), "application/json");
    const auto body = body_of(create);
    EXPECT_EQ(string_at(body, "name"), std::string(kRoom) + ":7");
    ASSERT_NE(body.find("max_participants"), nullptr);
    EXPECT_EQ(body.find("max_participants")->as_u64(), 2U);
    // Both cover a ticket's 60 s, and departure the SDK's 44 s of reconnect attempts.
    ASSERT_NE(body.find("empty_timeout"), nullptr);
    EXPECT_EQ(body.find("empty_timeout")->as_u64(), 60U);
    ASSERT_NE(body.find("departure_timeout"), nullptr);
    EXPECT_EQ(body.find("departure_timeout")->as_u64(), 60U);

    const auto claims = claims_of(create);
    EXPECT_EQ(string_at(claims, "iss"), "fake-key");
    EXPECT_EQ(bool_at(claims, "video", "roomCreate"), true);
    EXPECT_EQ(bool_at(claims, "video", "roomJoin"), std::nullopt);
}

TEST_P(LiveKitSfuTest, JoiningReopensTheGenerationBeforeIssuingItsTicket) {
    auto server = answering(200);
    start(server.base_url());
    auto room = open();
    ASSERT_TRUE(room);
    // Opened once; every join re-creates the room, as LiveKit may have dropped it since.
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(join(**room, "alice"));
    }
    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 3U);
    for (const ServedRequest& request : requests) {
        EXPECT_EQ(request.path(), "/twirp/livekit.RoomService/CreateRoom");
        EXPECT_EQ(request.body, requests[0].body) << "a re-created room has other settings";
    }
}

TEST_P(LiveKitSfuTest, JoiningIssuesATicketForThatUsersDevice) {
    auto server = answering(200);
    start(server.base_url());
    auto room = open();
    ASSERT_TRUE(room);
    const auto ticket = join(**room, "alice");
    ASSERT_TRUE(ticket);

    EXPECT_EQ(ticket->endpoint, "wss://media.example.test");
    const auto token = read_token(ticket->credential, kSecret);
    ASSERT_TRUE(token);
    EXPECT_EQ(string_at(token->claims, "sub"), "alice/" + std::string(kDevice));
    EXPECT_EQ(string_at(token->claims, "video", "room"), std::string(kRoom) + ":1");
    EXPECT_EQ(bool_at(token->claims, "video", "roomJoin"), true);
    EXPECT_EQ(bool_at(token->claims, "video", "roomCreate"), std::nullopt);
    EXPECT_EQ(ticket->expires_at, clock.wall_now() + std::chrono::seconds(60));
    EXPECT_EQ(token->claims.find("exp")->as_i64(),
              std::chrono::floor<core::Seconds>(ticket->expires_at).time_since_epoch().count());
}

TEST_P(LiveKitSfuTest, ATicketAdmitsToItsOwnGenerationOnly) {
    auto server = answering(200);
    start(server.base_url());
    auto first = open(MediaGeneration{1});
    auto second = open(MediaGeneration{2});
    ASSERT_TRUE(first && second);
    const auto old_ticket = join(**first, "alice");
    const auto new_ticket = join(**second, "alice");
    ASSERT_TRUE(old_ticket && new_ticket);
    EXPECT_EQ(string_at(read_token(old_ticket->credential, kSecret)->claims, "video", "room"),
              std::string(kRoom) + ":1");
    EXPECT_EQ(string_at(read_token(new_ticket->credential, kSecret)->claims, "video", "room"),
              std::string(kRoom) + ":2");
}

TEST_P(LiveKitSfuTest, TwoDevicesOfOneUserAreTwoParticipants) {
    auto server = answering(200);
    start(server.base_url());
    auto room = open();
    ASSERT_TRUE(room);
    const auto a = join(**room, "alice");
    const auto b = join(**room, "alice", "0192f3a4-0000-7000-8000-00000000000e");
    ASSERT_TRUE(a && b);
    EXPECT_NE(string_at(read_token(a->credential, kSecret)->claims, "sub"),
              string_at(read_token(b->credential, kSecret)->claims, "sub"));
}

TEST_P(LiveKitSfuTest, NoTicketIssuesForARoomThatCannotBeReopened) {
    std::atomic<int> served{0};
    HttpTestServer server([&](const ServedRequest&) {
        return ++served == 1
                   ? Reply{.status = 200, .headers = {}, .body = "{}"}
                   : Reply{.status = 503, .headers = {}, .body = R"({"code":"unavailable"})"};
    });
    start(server.base_url());
    auto room = open();
    ASSERT_TRUE(room);
    const auto ticket = join(**room, "alice");
    ASSERT_FALSE(ticket);
    EXPECT_EQ(ticket.error(), MediaError::Unavailable);
}

TEST_P(LiveKitSfuTest, AClosedRoomIssuesNoTicketAndIsNotRecreated) {
    auto server = answering(200);
    start(server.base_url());
    auto room = open();
    ASSERT_TRUE(room);
    ASSERT_TRUE(wait([&](auto done) { (*room)->close(std::move(done)); }));
    const auto ticket = join(**room, "alice");
    ASSERT_FALSE(ticket);
    EXPECT_EQ(ticket.error(), MediaError::Closed);
    EXPECT_EQ(server.request_count(), 2U) << "the closed generation was created again";
}

TEST_P(LiveKitSfuTest, ClosingAGenerationDeletesItsRoom) {
    auto server = answering(200);
    start(server.base_url());
    auto room = open(MediaGeneration{3});
    ASSERT_TRUE(room);
    ASSERT_TRUE(wait([&](auto done) { (*room)->close(std::move(done)); }));
    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 2U);
    EXPECT_EQ(requests[1].path(), "/twirp/livekit.RoomService/DeleteRoom");
    EXPECT_EQ(string_at(body_of(requests[1]), "room"), std::string(kRoom) + ":3");
    EXPECT_EQ(bool_at(claims_of(requests[1]), "video", "roomCreate"), true);
}

TEST_P(LiveKitSfuTest, ClosingWhatIsAlreadyGoneSucceeds) {
    std::atomic<int> served{0};
    // The room opens and has gone by the time it is closed.
    HttpTestServer server([&](const ServedRequest&) {
        return ++served == 1
                   ? Reply{.status = 200, .headers = {}, .body = "{}"}
                   : Reply{.status = 404, .headers = {}, .body = R"({"code":"not_found"})"};
    });
    start(server.base_url());
    auto room = open();
    ASSERT_TRUE(room);
    EXPECT_TRUE(wait([&](auto done) { (*room)->close(std::move(done)); }));
}

TEST_P(LiveKitSfuTest, OpeningFailsByWhetherARetryCanHelp) {
    {
        auto server = answering(401);
        start(server.base_url());
        const auto room = open();
        ASSERT_FALSE(room);
        EXPECT_EQ(room.error(), MediaError::Refused);
    }
    {
        auto server = answering(404);
        start(server.base_url());
        const auto room = open();
        ASSERT_FALSE(room);
        EXPECT_EQ(room.error(), MediaError::Refused) << "a create is never satisfied by absence";
    }
    {
        auto server = answering(503);
        start(server.base_url());
        const auto room = open();
        ASSERT_FALSE(room);
        EXPECT_EQ(room.error(), MediaError::Unavailable);
    }
}

TEST_P(LiveKitSfuTest, AServerThatIsNotListeningIsUnavailable) {
    // A port that was listened on and closed again refuses connections.
    auto socket = net::listen_tcp({.port = 0, .loopback_only = true, .reuse_port = false});
    ASSERT_TRUE(socket);
    const auto port = net::local_port(socket->get());
    ASSERT_TRUE(port);
    socket->reset();
    start("http://127.0.0.1:" + std::to_string(*port));
    const auto room = open();
    ASSERT_FALSE(room);
    EXPECT_EQ(room.error(), MediaError::Unavailable);
}

TEST_P(LiveKitSfuTest, DestroyingTheSfuDropsPendingCallbacks) {
    auto server = answering(200);
    start(server.base_url());
    int calls = 0;
    sfu->open_room(*core::RoomId::parse(kRoom), MediaGeneration{1}, 2,
                   [&](OpenResult) noexcept { ++calls; });
    sfu.reset();
    ulw::test::pump_for(*reactor, std::chrono::milliseconds(100));
    EXPECT_EQ(calls, 0);
}

TEST_P(LiveKitSfuTest, ACallbackMayDestroyTheSfu) {
    auto server = answering(200);
    start(server.base_url());
    bool called = false;
    sfu->open_room(*core::RoomId::parse(kRoom), MediaGeneration{1}, 2,
                   [&](OpenResult room) noexcept {
                       room->reset();
                       sfu.reset();
                       called = true;
                   });
    ASSERT_TRUE(pump_until(*reactor, [&] { return called; }));
    EXPECT_EQ(sfu, nullptr);
}

INSTANTIATE_TEST_SUITE_P(Reactors, LiveKitSfuTest,
                         ::testing::Values(net::ReactorKind::IoUring, net::ReactorKind::Epoll),
                         ulw::test::reactor_name);

class LiveKitConfigTest : public ::testing::Test {
protected:
    std::optional<ConfigError> error_of(Config config) {
        auto made = make_sfu(*reactor, *multi, clock, std::move(config));
        return made ? std::nullopt : std::optional(made.error());
    }

    void SetUp() override {
        auto r = net::make_reactor(net::ReactorKind::Epoll, clock, 64);
        ASSERT_TRUE(r);
        reactor = std::move(*r);
        auto m = infra::curl::Multi::create(*reactor);
        ASSERT_TRUE(m);
        multi = std::move(*m);
    }

    os::SystemClock clock;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<infra::curl::Multi> multi;
};

TEST_F(LiveKitConfigTest, AcceptsPlainAndSecureSchemes) {
    EXPECT_EQ(error_of(config_for("http://livekit:7880")), std::nullopt);
    auto secure = config_for("https://livekit.internal");
    secure.client_url = "ws://127.0.0.1:7880";
    EXPECT_EQ(error_of(secure), std::nullopt);
}

TEST_F(LiveKitConfigTest, RefusesWhatCouldNeverWork) {
    EXPECT_EQ(error_of(config_for("livekit:7880")), ConfigError::BadApiUrl);
    EXPECT_EQ(error_of(config_for("ws://livekit:7880")), ConfigError::BadApiUrl);
    EXPECT_EQ(error_of(config_for("http://")), ConfigError::BadApiUrl);

    auto client = config_for("http://livekit:7880");
    client.client_url = "https://media.example.test";
    EXPECT_EQ(error_of(client), ConfigError::BadClientUrl);

    auto key = config_for("http://livekit:7880");
    key.api_key.clear();
    EXPECT_EQ(error_of(key), ConfigError::MissingApiKey);

    auto secret = config_for("http://livekit:7880");
    secret.api_secret = std::string(31, 's');
    EXPECT_EQ(error_of(secret), ConfigError::BadApiSecret);
    secret.api_secret = std::string(32, 's');
    EXPECT_EQ(error_of(secret), std::nullopt);
    secret.api_secret = std::string(257, 's');
    EXPECT_EQ(error_of(secret), ConfigError::BadApiSecret);
}

} // namespace
