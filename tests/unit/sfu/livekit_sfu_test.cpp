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
#include <latch>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using core::ports::IMediaRoom;
using core::ports::ISfu;
using core::ports::MediaError;
using core::ports::MediaGeneration;
using core::ports::MediaRelay;
using core::ports::MediaRole;
using core::ports::MediaRoomKind;
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
using RelayResult = std::expected<std::string, MediaError>;

Config config_for(std::string api_url) {
    return Config{.api_url = std::move(api_url),
                  .client_url = "wss://media.example.test",
                  .api_key = "fake-key",
                  .api_secret = std::string(kSecret),
                  .packager_srt = "srt://live-{stream}.apps:9000"};
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

    OpenResult open(MediaGeneration generation = MediaGeneration{1},
                    MediaRoomKind kind = MediaRoomKind::Call) {
        std::optional<OpenResult> got;
        sfu->open_room(*core::RoomId::parse(kRoom), generation, kind, 2,
                       [&](OpenResult r) noexcept { got = std::move(r); });
        EXPECT_FALSE(got.has_value()) << "callback ran inside open_room()";
        EXPECT_TRUE(pump_until(*reactor, [&] { return got.has_value(); }));
        return std::move(*got);
    }

    TicketResult join(IMediaRoom& room, std::string_view user, std::string_view device = kDevice,
                      MediaRole role = MediaRole::Member) {
        std::optional<TicketResult> got;
        room.join(*core::UserId::parse(user), *core::DeviceId::parse(device), role,
                  [&](TicketResult r) noexcept { got = std::move(r); });
        EXPECT_FALSE(got.has_value()) << "callback ran inside join()";
        EXPECT_TRUE(pump_until(*reactor, [&] { return got.has_value(); }));
        return std::move(*got);
    }

    RelayResult relay(IMediaRoom& room, const MediaRelay& target) {
        std::optional<RelayResult> got;
        room.relay(*core::UserId::parse("streamer"), *core::DeviceId::parse(kDevice), target,
                   [&](RelayResult r) noexcept { got = std::move(r); });
        EXPECT_FALSE(got.has_value()) << "callback ran inside relay()";
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

TEST_P(LiveKitSfuTest, NoLimitIsSentAsLiveKitsZero) {
    auto server = answering(200);
    start(server.base_url());
    std::optional<OpenResult> got;
    sfu->open_room(*core::RoomId::parse(kRoom), MediaGeneration{1}, MediaRoomKind::Call, 0,
                   [&](OpenResult r) noexcept { got = std::move(r); });
    ASSERT_TRUE(pump_until(*reactor, [&] { return got.has_value(); }));
    ASSERT_TRUE(*got);
    // LiveKit reads 0 as unset, and a generation's room is only ever created with one setting.
    EXPECT_EQ(body_of(server.requests().at(0)).find("max_participants")->as_u64(), 0U);
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

TEST_P(LiveKitSfuTest, APublisherTicketIsForWhipAndCannotSubscribe) {
    auto server = answering(200);
    start(server.base_url());
    auto room = open(MediaGeneration{1}, MediaRoomKind::Stream);
    ASSERT_TRUE(room);
    const auto ticket = join(**room, "streamer", kDevice, MediaRole::Publisher);
    ASSERT_TRUE(ticket);
    EXPECT_EQ(ticket->endpoint, "https://media.example.test/whip/v1");
    const auto token = read_token(ticket->credential, kSecret);
    ASSERT_TRUE(token);
    EXPECT_EQ(string_at(token->claims, "sub"), "streamer/" + std::string(kDevice));
    EXPECT_EQ(string_at(token->claims, "video", "room"), std::string(kRoom) + ":1");
    EXPECT_EQ(bool_at(token->claims, "video", "canPublish"), true);
    EXPECT_EQ(bool_at(token->claims, "video", "canSubscribe"), false);
}

TEST_P(LiveKitSfuTest, APublisherTicketLivesNoLongerThanAMembers) {
    auto server = answering(200);
    start(server.base_url());
    auto room = open(MediaGeneration{1}, MediaRoomKind::Stream);
    ASSERT_TRUE(room);
    const auto publisher = join(**room, "streamer", kDevice, MediaRole::Publisher);
    ASSERT_TRUE(publisher);
    // A WHIP POST with it brings a closed generation's room back, so it must not outlive the
    // connect window; each PATCH and DELETE asks for a fresh one.
    EXPECT_EQ(publisher->expires_at, clock.wall_now() + std::chrono::seconds(60));
    const auto token = read_token(publisher->credential, kSecret);
    ASSERT_TRUE(token);
    EXPECT_EQ(token->claims.find("exp")->as_i64(),
              std::chrono::floor<core::Seconds>(publisher->expires_at).time_since_epoch().count());
}

constexpr std::string_view kEgressAnswer =
    R"({"egress_id":"EG_started","status":"EGRESS_STARTING"})";

// Answers the stream room's CreateRoom, ListEgress with `listed`, and the start with `started`.
HttpTestServer egress_server(std::string listed, int start_status = 200,
                             std::string started = std::string(kEgressAnswer)) {
    return HttpTestServer([listed = std::move(listed), start_status,
                           started = std::move(started)](const ServedRequest& request) {
        if (request.path().ends_with("/ListEgress")) {
            return Reply{.status = 200, .headers = {}, .body = listed};
        }
        if (request.path().ends_with("/StartParticipantEgress")) {
            return Reply{.status = start_status, .headers = {}, .body = started};
        }
        return Reply{.status = 200, .headers = {}, .body = "{}"};
    });
}

MediaRelay stream_target(std::string_view passphrase = "0123456789abcdef") {
    return MediaRelay{.stream = "s1",
                      .passphrase = std::string(passphrase),
                      .keyframe_interval = core::Seconds{2}};
}

TEST_P(LiveKitSfuTest, RelayingStartsARecorderToThatStreamsPackager) {
    auto server = egress_server(R"({"items":[]})");
    start(server.base_url());
    auto room = open(MediaGeneration{4}, MediaRoomKind::Stream);
    ASSERT_TRUE(room);
    const auto relayed = relay(**room, stream_target("pass word&=0123"));
    ASSERT_TRUE(relayed);
    EXPECT_EQ(*relayed, "EG_started");

    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 3U);
    EXPECT_EQ(requests[1].path(), "/twirp/livekit.Egress/ListEgress");
    const auto listed = body_of(requests[1]);
    EXPECT_EQ(string_at(listed, "room_name"), std::string(kRoom) + ":4");
    ASSERT_NE(listed.find("active"), nullptr);
    EXPECT_EQ(listed.find("active")->as_bool(), true);

    const ServedRequest& started = requests[2];
    EXPECT_EQ(started.path(), "/twirp/livekit.Egress/StartParticipantEgress");
    const auto body = body_of(started);
    EXPECT_EQ(string_at(body, "room_name"), std::string(kRoom) + ":4");
    EXPECT_EQ(string_at(body, "identity"), "streamer/" + std::string(kDevice));
    const core::json::Value* advanced = body.find("advanced");
    ASSERT_NE(advanced, nullptr);
    // The packager copies, so segments can only be cut where the recorder put keyframes.
    EXPECT_EQ(advanced->find("key_frame_interval")->as_u64(), 2U);
    EXPECT_EQ(advanced->find("height")->as_u64(), 720U);
    const auto* outputs = body.find("stream_outputs")->as_array();
    ASSERT_NE(outputs, nullptr);
    ASSERT_EQ(outputs->size(), 1U);
    EXPECT_EQ(string_at(outputs->front(), "protocol"), "SRT");
    const auto* urls = outputs->front().find("urls")->as_array();
    ASSERT_NE(urls, nullptr);
    ASSERT_EQ(urls->size(), 1U);
    // The address is the adapter's own; the passphrase cannot break out of its parameter.
    EXPECT_EQ(urls->front().as_string(),
              "srt://live-s1.apps:9000?streamid=s1&passphrase=pass%20word%26%3D0123");

    for (const ServedRequest& request : {requests[1], requests[2]}) {
        const auto claims = claims_of(request);
        EXPECT_EQ(bool_at(claims, "video", "roomRecord"), true);
        EXPECT_EQ(bool_at(claims, "video", "roomCreate"), std::nullopt);
        EXPECT_EQ(claims.find("sub"), nullptr);
    }
}

TEST_P(LiveKitSfuTest, ARelayAlreadyRunningIsReturnedAndNotStartedAgain) {
    // A retry after an answer that was lost: LiveKit runs the first attempt's relay.
    auto server = egress_server(
        R"({"items":[{"egress_id":"EG_other","status":"EGRESS_ACTIVE",)"
        R"("participant":{"identity":"someone/x"}},)"
        R"({"egress_id":"EG_running","status":"EGRESS_ACTIVE","participant":{"identity":"streamer/)" +
        std::string(kDevice) + R"("}}]})");
    start(server.base_url());
    auto room = open(MediaGeneration{1}, MediaRoomKind::Stream);
    ASSERT_TRUE(room);
    const auto relayed = relay(**room, stream_target());
    ASSERT_TRUE(relayed);
    EXPECT_EQ(*relayed, "EG_running");
    EXPECT_EQ(server.request_count(), 2U) << "a second relay was started";
}

TEST_P(LiveKitSfuTest, ARelayThatIsEndingIsNotTakenForTheRunningOne) {
    // ListEgress's `active` includes egresses on their way out, which will carry nothing more.
    auto server = egress_server(
        R"({"items":[{"egress_id":"EG_ending","status":"EGRESS_ENDING","participant":)"
        R"({"identity":"streamer/)" +
        std::string(kDevice) + R"("}}]})");
    start(server.base_url());
    auto room = open(MediaGeneration{1}, MediaRoomKind::Stream);
    ASSERT_TRUE(room);
    const auto relayed = relay(**room, stream_target());
    ASSERT_TRUE(relayed);
    EXPECT_EQ(*relayed, "EG_started");
    EXPECT_EQ(server.request_count(), 3U);
}

TEST_P(LiveKitSfuTest, RelaysAskedForTogetherStartOneRecorder) {
    // LiveKit lists an egress only once its start has answered; until then every listing is
    // empty, so the second call must wait for the first rather than list for itself.
    auto server = egress_server(R"({"items":[]})");
    start(server.base_url());
    auto room = open(MediaGeneration{1}, MediaRoomKind::Stream);
    ASSERT_TRUE(room);
    std::optional<RelayResult> first;
    std::optional<RelayResult> second;
    (*room)->relay(*core::UserId::parse("streamer"), *core::DeviceId::parse(kDevice),
                   stream_target(), [&](RelayResult r) noexcept { first = std::move(r); });
    (*room)->relay(*core::UserId::parse("streamer"), *core::DeviceId::parse(kDevice),
                   stream_target(), [&](RelayResult r) noexcept { second = std::move(r); });
    ASSERT_TRUE(pump_until(*reactor, [&] { return first && second; }));
    ASSERT_TRUE(*first && *second);
    EXPECT_EQ(**first, "EG_started");
    EXPECT_EQ(**second, "EG_started");
    int starts = 0;
    for (const ServedRequest& request : server.requests()) {
        starts += request.path().ends_with("/StartParticipantEgress") ? 1 : 0;
    }
    EXPECT_EQ(starts, 1);
    // Once answered, a later call lists again rather than waiting on what is over.
    ASSERT_TRUE(relay(**room, stream_target()));
    EXPECT_EQ(server.request_count(), 5U);
}

TEST_P(LiveKitSfuTest, APackagerHostNamedByStreamTakesOnlyDnsLabels) {
    auto server = egress_server(R"({"items":[]})");
    start(server.base_url());
    auto room = open(MediaGeneration{1}, MediaRoomKind::Stream);
    ASSERT_TRUE(room);
    // Valid as a stream id, not as a host name: uppercase, '_' and 64 characters.
    for (const std::string& id :
         {std::string("Stream1"), std::string("s_1"), std::string(64, 's')}) {
        auto target = stream_target();
        target.stream = id;
        EXPECT_EQ(relay(**room, target), RelayResult(std::unexpected(MediaError::Refused))) << id;
    }
    EXPECT_EQ(server.request_count(), 1U);
}

TEST_P(LiveKitSfuTest, RelayingFailsByWhetherARetryCanHelp) {
    {
        // No recorder is connected to LiveKit, or none has the CPU to spare.
        auto server = egress_server(R"({"items":[]})", 503);
        start(server.base_url());
        auto room = open(MediaGeneration{1}, MediaRoomKind::Stream);
        ASSERT_TRUE(room);
        const auto busy = relay(**room, stream_target());
        ASSERT_FALSE(busy);
        EXPECT_EQ(busy.error(), MediaError::Unavailable);
    }
    {
        auto server = egress_server(R"({"items":[]})", 400);
        start(server.base_url());
        auto room = open(MediaGeneration{1}, MediaRoomKind::Stream);
        ASSERT_TRUE(room);
        const auto rejected = relay(**room, stream_target());
        ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.error(), MediaError::Refused);
    }
    {
        // Started, perhaps, but with no id to show for it: the retry's listing will find it.
        auto server = egress_server(R"({"items":[]})", 200, "{}");
        start(server.base_url());
        auto room = open(MediaGeneration{1}, MediaRoomKind::Stream);
        ASSERT_TRUE(room);
        const auto unread = relay(**room, stream_target());
        ASSERT_FALSE(unread);
        EXPECT_EQ(unread.error(), MediaError::Unavailable);
    }
}

TEST_P(LiveKitSfuTest, ACallsRoomIsNeverRelayed) {
    auto server = egress_server(R"({"items":[]})");
    start(server.base_url());
    auto room = open(MediaGeneration{1}, MediaRoomKind::Call);
    ASSERT_TRUE(room);
    const auto relayed = relay(**room, stream_target());
    ASSERT_FALSE(relayed);
    EXPECT_EQ(relayed.error(), MediaError::Refused);
    EXPECT_EQ(server.request_count(), 1U);
}

TEST_P(LiveKitSfuTest, ARelayThePackagerCouldNotTakeIsRefusedUnsent) {
    auto server = egress_server(R"({"items":[]})");
    start(server.base_url());
    auto room = open(MediaGeneration{1}, MediaRoomKind::Stream);
    ASSERT_TRUE(room);
    auto target = stream_target();
    // Segment lengths outside the packager's 2 to 10 s.
    target.keyframe_interval = core::Seconds{1};
    EXPECT_EQ(relay(**room, target), RelayResult(std::unexpected(MediaError::Refused)));
    target.keyframe_interval = core::Seconds{11};
    EXPECT_EQ(relay(**room, target), RelayResult(std::unexpected(MediaError::Refused)));
    // A stream id that is not one key segment, and a passphrase SRT refuses.
    EXPECT_EQ(relay(**room, MediaRelay{.stream = "s1/../s2",
                                       .passphrase = "0123456789",
                                       .keyframe_interval = core::Seconds{2}}),
              RelayResult(std::unexpected(MediaError::Refused)));
    EXPECT_EQ(relay(**room, stream_target("short")),
              RelayResult(std::unexpected(MediaError::Refused)));
    EXPECT_EQ(server.request_count(), 1U);
}

TEST_P(LiveKitSfuTest, WithNoPackagerConfiguredNothingIsRelayed) {
    auto server = egress_server(R"({"items":[]})");
    auto config = config_for(server.base_url());
    config.packager_srt.clear();
    auto made = make_sfu(*reactor, *multi, clock, std::move(config));
    ASSERT_TRUE(made);
    sfu = std::move(*made);
    auto room = open(MediaGeneration{1}, MediaRoomKind::Stream);
    ASSERT_TRUE(room);
    EXPECT_EQ(relay(**room, stream_target()), RelayResult(std::unexpected(MediaError::Refused)));
    EXPECT_EQ(server.request_count(), 1U);
}

TEST_P(LiveKitSfuTest, AClosedRoomRelaysNothing) {
    auto server = egress_server(R"({"items":[]})");
    start(server.base_url());
    auto room = open(MediaGeneration{1}, MediaRoomKind::Stream);
    ASSERT_TRUE(room);
    ASSERT_TRUE(wait([&](auto done) { (*room)->close(std::move(done)); }));
    const auto relayed = relay(**room, stream_target());
    ASSERT_FALSE(relayed);
    EXPECT_EQ(relayed.error(), MediaError::Closed);
    EXPECT_EQ(server.request_count(), 2U);
}

TEST_P(LiveKitSfuTest, ThePlainClientUrlGivesAPlainWhipUrl) {
    auto server = answering(200);
    auto config = config_for(server.base_url());
    config.client_url = "ws://127.0.0.1:7880/";
    auto made = make_sfu(*reactor, *multi, clock, std::move(config));
    ASSERT_TRUE(made);
    sfu = std::move(*made);
    auto call = open(MediaGeneration{1}, MediaRoomKind::Call);
    auto stream = open(MediaGeneration{2}, MediaRoomKind::Stream);
    ASSERT_TRUE(call && stream);
    const auto publisher = join(**stream, "streamer", kDevice, MediaRole::Publisher);
    const auto member = join(**call, "alice");
    ASSERT_TRUE(publisher && member);
    EXPECT_EQ(publisher->endpoint, "http://127.0.0.1:7880/whip/v1");
    EXPECT_EQ(member->endpoint, "ws://127.0.0.1:7880/");
}

TEST_P(LiveKitSfuTest, ARoomIssuesOnlyTheTicketsOfItsKind) {
    auto server = answering(200);
    start(server.base_url());
    auto call = open(MediaGeneration{1}, MediaRoomKind::Call);
    auto stream = open(MediaGeneration{2}, MediaRoomKind::Stream);
    ASSERT_TRUE(call && stream);
    // A publisher ticket would let its holder bring a closed call generation back and rejoin
    // it; members never join a stream's room.
    const auto publisher = join(**call, "streamer", kDevice, MediaRole::Publisher);
    ASSERT_FALSE(publisher);
    EXPECT_EQ(publisher.error(), MediaError::Refused);
    const auto member = join(**stream, "alice");
    ASSERT_FALSE(member);
    EXPECT_EQ(member.error(), MediaError::Refused);
    EXPECT_EQ(server.request_count(), 2U) << "a refused join re-created its room";
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
    const HttpTestServer server([&](const ServedRequest&) {
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

using Listed = std::expected<std::vector<core::ports::MediaParticipant>, MediaError>;

Listed list_participants(net::IReactor& reactor, IMediaRoom& room) {
    std::optional<Listed> got;
    room.participants([&](Listed r) noexcept { got = std::move(r); });
    EXPECT_FALSE(got.has_value()) << "callback ran inside participants()";
    EXPECT_TRUE(pump_until(reactor, [&] { return got.has_value(); }));
    return got.value_or(std::unexpected(MediaError::Unavailable));
}

TEST_P(LiveKitSfuTest, ParticipantsAreTheMembersConnectedToTheGeneration) {
    // LiveKit's protojson writes int64 as strings; a number is read as well.
    const HttpTestServer server([](const ServedRequest& request) {
        if (request.path() != "/twirp/livekit.RoomService/ListParticipants") {
            return Reply{.status = 200, .headers = {}, .body = "{}"};
        }
        return Reply{.status = 200,
                     .headers = {},
                     .body = std::string(R"({"participants":[)") +
                             R"({"identity":"alice/)" + std::string(kDevice) +
                             R"(","state":"ACTIVE","joined_at":"1790000000","joined_at_ms":"1790000000123"},)" +
                             R"({"identity":"bob/0192f3a4-0000-7000-8000-00000000000e","state":"JOINED","joined_at":1790000001},)" +
                             R"({"identity":"carol/0192f3a4-0000-7000-8000-00000000000f","state":"DISCONNECTED"},)" +
                             R"({"identity":"EG_recorder","state":"ACTIVE"},)" +
                             R"({"identity":"mallory/not-a-uuid","state":"ACTIVE"}]})"};
    });
    start(server.base_url());
    auto room = open(MediaGeneration{5});
    ASSERT_TRUE(room);
    const Listed listed = list_participants(*reactor, **room);
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed->size(), 2U);
    EXPECT_EQ((*listed)[0].user, *core::UserId::parse("alice"));
    EXPECT_EQ((*listed)[0].device, *core::DeviceId::parse(kDevice));
    EXPECT_EQ((*listed)[0].joined_at, core::WallTime{core::Millis{1'790'000'000'123}});
    EXPECT_EQ((*listed)[1].user, *core::UserId::parse("bob"));
    EXPECT_EQ((*listed)[1].joined_at, core::WallTime{core::Millis{1'790'000'001'000}});

    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 2U);
    const ServedRequest& list = requests[1];
    EXPECT_EQ(string_at(body_of(list), "room"), std::string(kRoom) + ":5");
    // Its token reads that one room, and does nothing else.
    const auto claims = claims_of(list);
    EXPECT_EQ(bool_at(claims, "video", "roomAdmin"), true);
    EXPECT_EQ(string_at(claims, "video", "room"), std::string(kRoom) + ":5");
    EXPECT_EQ(bool_at(claims, "video", "roomCreate"), std::nullopt);
    EXPECT_EQ(bool_at(claims, "video", "roomJoin"), std::nullopt);
}

TEST_P(LiveKitSfuTest, ARoomLiveKitDroppedHoldsNobody) {
    std::atomic<int> served{0};
    const HttpTestServer server([&](const ServedRequest&) {
        return ++served == 1
                   ? Reply{.status = 200, .headers = {}, .body = "{}"}
                   : Reply{.status = 404, .headers = {}, .body = R"({"code":"not_found"})"};
    });
    start(server.base_url());
    auto room = open();
    ASSERT_TRUE(room);
    const Listed listed = list_participants(*reactor, **room);
    ASSERT_TRUE(listed);
    EXPECT_TRUE(listed->empty());
}

TEST_P(LiveKitSfuTest, ListingParticipantsFailsByWhetherARetryCanHelp) {
    for (const auto& [status, body, error] :
         std::vector<std::tuple<int, std::string, MediaError>>{
             {503, R"({"code":"unavailable"})", MediaError::Unavailable},
             {401, R"({"code":"unauthenticated"})", MediaError::Refused},
             // A 404 that is not LiveKit's not_found: a route that is not there.
             {404, R"({"code":"bad_route"})", MediaError::Refused},
             {200, "not json", MediaError::Unavailable}}) {
        std::atomic<int> served{0};
        const HttpTestServer server([&, status = status, body = body](const ServedRequest&) {
            return ++served == 1 ? Reply{.status = 200, .headers = {}, .body = "{}"}
                                 : Reply{.status = status, .headers = {}, .body = body};
        });
        start(server.base_url());
        auto room = open();
        ASSERT_TRUE(room);
        const Listed listed = list_participants(*reactor, **room);
        ASSERT_FALSE(listed) << status;
        EXPECT_EQ(listed.error(), error) << status;
        room->reset();
        sfu.reset();
    }
}

TEST_P(LiveKitSfuTest, AClosedRoomListsNobodyAndAsksNothing) {
    auto server = answering(200);
    start(server.base_url());
    auto room = open();
    ASSERT_TRUE(room);
    ASSERT_TRUE(wait([&](auto done) { (*room)->close(std::move(done)); }));
    const Listed listed = list_participants(*reactor, **room);
    ASSERT_FALSE(listed);
    EXPECT_EQ(listed.error(), MediaError::Closed);
    EXPECT_EQ(server.requests().size(), 2U) << "only the create and the delete";
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
    const HttpTestServer server([&](const ServedRequest&) {
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

// libcurl refuses a URL longer than CURL_MAX_INPUT_LENGTH (8,000,000 bytes) when it is set, so
// the request fails before anything reaches the network.
std::string unsendable_url() {
    return "http://127.0.0.1/" + std::string(std::size_t{8'000'001}, 'a');
}

TEST_P(LiveKitSfuTest, ARequestThatCannotBeSentFailsLaterAsRefused) {
    start(unsendable_url());
    const auto room = open();
    ASSERT_FALSE(room);
    EXPECT_EQ(room.error(), MediaError::Refused);
}

TEST_P(LiveKitSfuTest, ARequestThatCannotBeSentIsDroppedWithTheSfu) {
    start(unsendable_url());
    int calls = 0;
    sfu->open_room(*core::RoomId::parse(kRoom), MediaGeneration{1}, MediaRoomKind::Call, 2,
                   [&](OpenResult) noexcept { ++calls; });
    EXPECT_EQ(calls, 0) << "callback ran inside open_room()";
    sfu.reset();
    ulw::test::pump_pending(*reactor);
    EXPECT_EQ(calls, 0);
}

TEST_P(LiveKitSfuTest, DestroyingTheSfuDropsPendingCallbacks) {
    // The server holds its answer until the sfu is gone, then answers anyway.
    std::atomic<bool> arrived{false};
    std::latch release{1};
    HttpTestServer server([&](const ServedRequest&) {
        arrived = true;
        release.wait();
        return Reply{.status = 200, .headers = {}, .body = "{}"};
    });
    // Destroyed before the server, so a failed assertion cannot leave its thread waiting.
    const struct Releaser {
        explicit Releaser(std::latch& l) noexcept : latch(l) {}
        Releaser(const Releaser&) = delete;
        Releaser& operator=(const Releaser&) = delete;
        Releaser(Releaser&&) = delete;
        Releaser& operator=(Releaser&&) = delete;
        ~Releaser() {
            if (!latch.try_wait()) {
                latch.count_down();
            }
        }
        std::latch& latch;
    } releaser{release};
    start(server.base_url());
    int calls = 0;
    sfu->open_room(*core::RoomId::parse(kRoom), MediaGeneration{1}, MediaRoomKind::Call, 2,
                   [&](OpenResult) noexcept { ++calls; });
    ASSERT_TRUE(pump_until(*reactor, [&] { return arrived.load(); }));
    sfu.reset();
    release.count_down();
    // Recorded once the handler has answered; one more turn delivers anything the answer set off.
    ASSERT_TRUE(pump_until(*reactor, [&] { return server.request_count() == 1; }));
    ulw::test::pump_pending(*reactor);
    EXPECT_EQ(calls, 0);
}

TEST_P(LiveKitSfuTest, ACallbackMayDestroyTheSfu) {
    auto server = answering(200);
    start(server.base_url());
    bool called = false;
    sfu->open_room(*core::RoomId::parse(kRoom), MediaGeneration{1}, MediaRoomKind::Call, 2,
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

TEST_F(LiveKitConfigTest, APackagerAddressIsSrtHostAndPortOnly) {
    auto config = config_for("http://livekit:7880");
    for (const std::string_view good :
         {"", "srt://127.0.0.1:9000", "srt://live-{stream}.apps:9000"}) {
        config.packager_srt = good;
        EXPECT_EQ(error_of(config), std::nullopt) << good;
    }
    for (const std::string_view bad :
         {"rtmp://packager:1935", "srt://packager", "srt://:9000", "srt://packager:99999",
          "srt://packager:9000?streamid=x", "srt://packager:9000/x"}) {
        config.packager_srt = bad;
        EXPECT_EQ(error_of(config), ConfigError::BadPackagerAddress) << bad;
    }
}

} // namespace
