// External TURN (ADR-0109): its settings, both providers' credentials, and the media port that
// hands them out with every ticket.
#include "core/models/ids.hpp"
#include "core/ports/media.hpp"
#include "core/ports/turn.hpp"
#include "infra/turn/turn.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"

#include "support/fake_clock.hpp"
#include "support/http_test_server.hpp"
#include "support/reactor_harness.hpp"

#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using core::ports::IceServer;
using core::ports::IMediaRoom;
using core::ports::ISfu;
using core::ports::ITurnCredentials;
using core::ports::MediaError;
using core::ports::MediaGeneration;
using core::ports::MediaRole;
using core::ports::MediaRoomKind;
using core::ports::MediaTicket;
using infra::turn::Config;
using infra::turn::load_config;
using infra::turn::Provider;
using ulw::test::HttpTestServer;
using ulw::test::pump_until;
using ulw::test::Reply;
using ulw::test::ServedRequest;

constexpr std::string_view kSecret = "turn-shared-secret-0123456789";
constexpr std::string_view kDevice = "0192f3a4-0000-7000-8000-00000000000d";
constexpr std::string_view kRoom = "0192f3a4-0000-7000-8000-000000000001";

using Env = std::map<std::string, std::string, std::less<>>;
using Servers = std::expected<std::vector<IceServer>, MediaError>;
using TicketResult = std::expected<MediaTicket, MediaError>;

auto loaded(const Env& env) {
    return load_config([&](std::string_view name) -> std::optional<std::string> {
        const auto it = env.find(name);
        return it == env.end() ? std::nullopt : std::optional(it->second);
    });
}

std::string error_variable(const Env& env) {
    const auto config = loaded(env);
    return config ? std::string{} : config.error().variable;
}

Env hmac_env() {
    return {{"ULW_TURN_MODE", "external"},
            {"ULW_TURN_PROVIDER", "hmac"},
            {"ULW_TURN_URLS", "turn:relay.example.com:3478?transport=udp,"
                              "turns:relay.example.com:5349?transport=tcp"},
            {"ULW_TURN_SHARED_SECRET", std::string(kSecret)}};
}

Env cloudflare_env() {
    return {{"ULW_TURN_MODE", "external"},
            {"ULW_TURN_PROVIDER", "cloudflare"},
            {"ULW_TURN_KEY_ID", "0123abcd"},
            {"ULW_TURN_KEY_API_TOKEN", "token-of-the-key"}};
}

TEST(TurnConfig, SelfHostedIsTheDefaultAndTakesNothingElse) {
    const auto unset = loaded({});
    ASSERT_TRUE(unset);
    EXPECT_FALSE(unset->has_value());
    const auto named = loaded({{"ULW_TURN_MODE", "self-hosted"}});
    ASSERT_TRUE(named);
    EXPECT_FALSE(named->has_value());
    // Half a configuration would read as media leaving through a relay it does not use.
    EXPECT_EQ(error_variable({{"ULW_TURN_PROVIDER", "hmac"}}), "ULW_TURN_PROVIDER");
    EXPECT_EQ(error_variable({{"ULW_TURN_MODE", "self-hosted"},
                              {"ULW_TURN_SHARED_SECRET", std::string(kSecret)}}),
              "ULW_TURN_SHARED_SECRET");
    // Empty is unset, as an overlay writes a key it does not use.
    EXPECT_TRUE(loaded({{"ULW_TURN_MODE", ""}, {"ULW_TURN_PROVIDER", ""}}));
    EXPECT_EQ(error_variable({{"ULW_TURN_MODE", "managed"}}), "ULW_TURN_MODE");
}

TEST(TurnConfig, HmacTakesItsUrlsAndSecret) {
    const auto config = loaded(hmac_env());
    ASSERT_TRUE(config) << config.error().variable << ": " << config.error().reason;
    ASSERT_TRUE(config->has_value());
    const Config& c = **config;
    EXPECT_EQ(c.provider, Provider::Hmac);
    EXPECT_EQ(c.urls, (std::vector<std::string>{"turn:relay.example.com:3478?transport=udp",
                                                "turns:relay.example.com:5349?transport=tcp"}));
    EXPECT_EQ(c.shared_secret, kSecret);
    EXPECT_EQ(c.ttl, core::Seconds{14'400});
}

TEST(TurnConfig, HmacRefusesWhatCannotWork) {
    auto env = hmac_env();
    env.erase("ULW_TURN_SHARED_SECRET");
    EXPECT_EQ(error_variable(env), "ULW_TURN_SHARED_SECRET");
    env = hmac_env();
    env["ULW_TURN_SHARED_SECRET"] = "short";
    EXPECT_EQ(error_variable(env), "ULW_TURN_SHARED_SECRET");
    for (const std::string urls : {"", "https://relay.example.com", "turn:", "stun:a.example:3478",
                                   "turn:a.example:3478,,turn:b.example:3478"}) {
        env = hmac_env();
        env["ULW_TURN_URLS"] = urls;
        EXPECT_EQ(error_variable(env), "ULW_TURN_URLS") << urls;
    }
    env = hmac_env();
    env["ULW_TURN_KEY_ID"] = "0123abcd";
    EXPECT_EQ(error_variable(env), "ULW_TURN_KEY_ID");
}

TEST(TurnConfig, CloudflareTakesItsKeyAndToken) {
    const auto config = loaded(cloudflare_env());
    ASSERT_TRUE(config && config->has_value());
    const Config& c = **config;
    EXPECT_EQ(c.provider, Provider::Cloudflare);
    EXPECT_EQ(c.key_id, "0123abcd");
    EXPECT_EQ(c.api_token, "token-of-the-key");
    EXPECT_EQ(c.api_url, "https://rtc.live.cloudflare.com");

    auto env = cloudflare_env();
    env.erase("ULW_TURN_KEY_API_TOKEN");
    EXPECT_EQ(error_variable(env), "ULW_TURN_KEY_API_TOKEN");
    env = cloudflare_env();
    env["ULW_TURN_KEY_ID"] = "../keys";
    EXPECT_EQ(error_variable(env), "ULW_TURN_KEY_ID");
    // The token never crosses a network in the clear.
    env = cloudflare_env();
    env["ULW_TURN_API_URL"] = "http://api.example.com";
    EXPECT_EQ(error_variable(env), "ULW_TURN_API_URL");
    env["ULW_TURN_API_URL"] = "http://127.0.0.1:8080";
    EXPECT_TRUE(loaded(env));
    env = cloudflare_env();
    env["ULW_TURN_SHARED_SECRET"] = std::string(kSecret);
    EXPECT_EQ(error_variable(env), "ULW_TURN_SHARED_SECRET");
}

TEST(TurnConfig, TheLifetimeIsBounded) {
    auto env = hmac_env();
    env["ULW_TURN_TTL_SECONDS"] = "86400";
    const auto config = loaded(env);
    ASSERT_TRUE(config && config->has_value());
    EXPECT_EQ((*config)->ttl, core::Seconds{86'400});
    for (const std::string ttl : {"599", "172801", "4h", "-1"}) {
        env["ULW_TURN_TTL_SECONDS"] = ttl;
        EXPECT_EQ(error_variable(env), "ULW_TURN_TTL_SECONDS") << ttl;
    }
    env = hmac_env();
    env["ULW_TURN_PROVIDER"] = "twilio";
    EXPECT_EQ(error_variable(env), "ULW_TURN_PROVIDER");
}

TEST(HmacServer, IsTheTurnRestApisCredential) {
    // base64(HMAC-SHA1(secret, "<expiry>:<name>")), as coturn's use-auth-secret checks it.
    const IceServer server = infra::turn::detail::hmac_server({"turn:relay.example.com:3478"},
                                                              kSecret, kDevice, 1'790'000'000);
    EXPECT_EQ(server.urls, (std::vector<std::string>{"turn:relay.example.com:3478"}));
    EXPECT_EQ(server.username, "1790000000:" + std::string(kDevice));
    EXPECT_EQ(server.credential, "aJ2r7zhGXLkDzqolZiNhr8T2zI8=");
}

TEST(ParseCloudflare, ReadsItsIceServersWithoutPort53) {
    const auto servers = infra::turn::detail::parse_cloudflare(R"({"iceServers":[
        {"urls":["stun:stun.cloudflare.com:3478","stun:stun.cloudflare.com:53"]},
        {"urls":["turn:turn.cloudflare.com:3478?transport=udp",
                 "turn:turn.cloudflare.com:53?transport=udp",
                 "turns:turn.cloudflare.com:443?transport=tcp"],
         "username":"u-1","credential":"c-1"}]})");
    ASSERT_TRUE(servers);
    ASSERT_EQ(servers->size(), 2U);
    EXPECT_EQ(
        (*servers)[0],
        (IceServer{.urls = {"stun:stun.cloudflare.com:3478"}, .username = {}, .credential = {}}));
    EXPECT_EQ((*servers)[1], (IceServer{.urls = {"turn:turn.cloudflare.com:3478?transport=udp",
                                                 "turns:turn.cloudflare.com:443?transport=tcp"},
                                        .username = "u-1",
                                        .credential = "c-1"}));
}

TEST(ParseCloudflare, TakesTheFirstVersionsSingleObject) {
    const auto servers = infra::turn::detail::parse_cloudflare(
        R"({"iceServers":{"urls":"turn:turn.cloudflare.com:3478","username":"u","credential":"c"}})");
    ASSERT_TRUE(servers);
    ASSERT_EQ(servers->size(), 1U);
    EXPECT_EQ((*servers)[0].urls, (std::vector<std::string>{"turn:turn.cloudflare.com:3478"}));
}

TEST(ParseCloudflare, RefusesAnAnswerWithNoRelay) {
    for (const std::string_view body :
         {R"({})", R"(not json)", R"({"iceServers":[{"urls":["stun:stun.cloudflare.com:3478"]}]})",
          R"({"iceServers":[{"urls":["turn:turn.cloudflare.com:3478"]}]})",
          R"({"iceServers":[{"urls":[3478],"username":"u","credential":"c"}]})"}) {
        EXPECT_FALSE(infra::turn::detail::parse_cloudflare(body)) << body;
    }
}

// The issuers and the port, on a real reactor.
class TurnTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto r = net::make_reactor(net::ReactorKind::Epoll, reactor_clock, 1024);
        ASSERT_TRUE(r);
        reactor = std::move(*r);
        auto m = infra::curl::Multi::create(*reactor);
        ASSERT_TRUE(m);
        multi = std::move(*m);
    }
    void TearDown() override {
        issuer.reset();
        multi.reset();
        reactor.reset();
    }

    void make(const Env& env) {
        auto config = loaded(env);
        ASSERT_TRUE(config && config->has_value());
        issuer = infra::turn::make_credentials(*reactor, *multi, clock, std::move(**config));
    }

    Servers issue() {
        std::optional<Servers> got;
        issuer->issue(*core::UserId::parse("caller"), *core::DeviceId::parse(kDevice),
                      [&](Servers r) noexcept { got = std::move(r); });
        EXPECT_FALSE(got.has_value()) << "callback ran inside issue()";
        EXPECT_TRUE(pump_until(*reactor, [&] { return got.has_value(); }));
        return got.value_or(std::unexpected(MediaError::Unavailable));
    }

    os::SystemClock reactor_clock;
    ulw::test::FakeClock clock;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<infra::curl::Multi> multi;
    std::unique_ptr<ITurnCredentials> issuer;
};

TEST_F(TurnTest, HmacMintsForTheDeviceUntilTheLifetimeEnds) {
    make(hmac_env());
    const Servers servers = issue();
    ASSERT_TRUE(servers);
    ASSERT_EQ(servers->size(), 1U);
    const IceServer& server = servers->front();
    EXPECT_EQ(server.urls.size(), 2U);
    // 2026-01-01T00:00:00Z plus four hours, and the device rather than the user.
    EXPECT_EQ(server.username, "1767240000:" + std::string(kDevice));
    EXPECT_EQ(
        server.credential,
        infra::turn::detail::hmac_server(server.urls, kSecret, kDevice, 1'767'240'000).credential);
}

TEST_F(TurnTest, CloudflareAsksItsApiWithTheKey) {
    const HttpTestServer api([](const ServedRequest&) {
        return Reply{.status = 201,
                     .headers = {{"Content-Type", "application/json"}},
                     .body = R"({"iceServers":[{"urls":["turn:turn.cloudflare.com:3478"],)"
                             R"("username":"u-1","credential":"c-1"}]})"};
    });
    auto env = cloudflare_env();
    env["ULW_TURN_API_URL"] = api.base_url();
    env["ULW_TURN_TTL_SECONDS"] = "3600";
    make(env);
    const Servers servers = issue();
    ASSERT_TRUE(servers);
    ASSERT_EQ(servers->size(), 1U);
    EXPECT_EQ(servers->front().username, "u-1");
    const auto requests = api.requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests[0].method, "POST");
    EXPECT_EQ(requests[0].path(), "/v1/turn/keys/0123abcd/credentials/generate-ice-servers");
    EXPECT_EQ(requests[0].header("authorization"), "Bearer token-of-the-key");
    EXPECT_EQ(requests[0].body, R"({"ttl":3600})");
}

TEST_F(TurnTest, CloudflaresRefusalsAndOutagesAreTold) {
    for (const auto& [status, body, expected] :
         {std::tuple{401, std::string(R"({"error":"unauthorized"})"), MediaError::Refused},
          std::tuple{404, std::string(), MediaError::Refused},
          std::tuple{429, std::string(), MediaError::Unavailable},
          std::tuple{503, std::string(), MediaError::Unavailable},
          std::tuple{201, std::string(R"({"iceServers":[]})"), MediaError::Unavailable}}) {
        const HttpTestServer api([status, body](const ServedRequest&) {
            return Reply{.status = status, .headers = {}, .body = body};
        });
        auto env = cloudflare_env();
        env["ULW_TURN_API_URL"] = api.base_url();
        make(env);
        const Servers servers = issue();
        ASSERT_FALSE(servers) << status;
        EXPECT_EQ(servers.error(), expected) << status;
        issuer.reset();
    }
}

TEST_F(TurnTest, CloudflareUnreachableIsUnavailableAndAProtocolItCannotSpeakIsRefused) {
    Config config{};
    config.provider = Provider::Cloudflare;
    config.key_id = "0123abcd";
    config.api_token = "token-of-the-key";
    // Nothing listens on port 1 of this machine.
    config.api_url = "http://127.0.0.1:1";
    issuer = infra::turn::make_credentials(*reactor, *multi, clock, config);
    Servers servers = issue();
    ASSERT_FALSE(servers);
    EXPECT_EQ(servers.error(), MediaError::Unavailable);
    // A scheme libcurl does not speak: the request cannot be made at all.
    config.api_url = "gopherx://127.0.0.1";
    issuer = infra::turn::make_credentials(*reactor, *multi, clock, config);
    servers = issue();
    ASSERT_FALSE(servers);
    EXPECT_EQ(servers.error(), MediaError::Refused);
}

TEST(ParseCloudflare, RefusesEntriesOfTheWrongShape) {
    for (const std::string_view body :
         {R"({"iceServers":[{"username":"u","credential":"c"}]})",
          R"({"iceServers":[{"urls":"turn:t:3478","username":1,"credential":"c"}]})",
          R"({"iceServers":[{"urls":{"a":1},"username":"u","credential":"c"}]})",
          R"({"iceServers":"turn:t:3478"})"}) {
        EXPECT_FALSE(infra::turn::detail::parse_cloudflare(body)) << body;
    }
    // Only port 53: an entry with nothing left is dropped, and with it the only relay.
    EXPECT_FALSE(infra::turn::detail::parse_cloudflare(
        R"({"iceServers":{"urls":"turn:t:53?transport=udp","username":"u","credential":"c"}})"));
    // An IPv6 host's last colon is inside its brackets when it names no port.
    const auto bracketed = infra::turn::detail::parse_cloudflare(
        R"({"iceServers":[{"urls":["turn:[2001:db8::53]"],"username":"u","credential":"c"}]})");
    ASSERT_TRUE(bracketed);
    EXPECT_EQ(bracketed->front().urls.size(), 1U);
}

TEST(TurnConfig, DescribesItsModeWithoutACredential) {
    EXPECT_EQ(infra::turn::describe(std::nullopt), "self-hosted");
    Config config{};
    EXPECT_EQ(infra::turn::describe(config), "external hmac");
    config.provider = Provider::Cloudflare;
    EXPECT_EQ(infra::turn::describe(config), "external cloudflare");
}

TEST(TurnConfig, RefusesWhatTheOtherProviderTakes) {
    auto env = cloudflare_env();
    env["ULW_TURN_URLS"] = "turn:relay.example.com:3478";
    EXPECT_EQ(error_variable(env), "ULW_TURN_URLS");
    env = cloudflare_env();
    env.erase("ULW_TURN_KEY_ID");
    EXPECT_EQ(error_variable(env), "ULW_TURN_KEY_ID");
    env = hmac_env();
    std::string many;
    for (int i = 0; i < 17; ++i) {
        many += (i == 0 ? "" : ",") + std::string("turn:relay.example.com:3478");
    }
    env["ULW_TURN_URLS"] = many;
    EXPECT_EQ(error_variable(env), "ULW_TURN_URLS");
    env = hmac_env();
    env.erase("ULW_TURN_URLS");
    EXPECT_EQ(error_variable(env), "ULW_TURN_URLS");
}

TEST(AppendIceServers, WritesNothingForNoServers) {
    std::string out = "x";
    infra::turn::append_ice_servers(out, {});
    EXPECT_EQ(out, "x");
}

// A media server that admits whoever it is told to.
class FakeRoom final : public IMediaRoom {
public:
    FakeRoom(net::IReactor& reactor, std::optional<MediaError> refuse) noexcept
        : reactor_(reactor), refuse_(refuse) {}
    ~FakeRoom() override {
        if (timer_) {
            reactor_.cancel_timer(*timer_);
        }
    }
    FakeRoom(const FakeRoom&) = delete;
    FakeRoom& operator=(const FakeRoom&) = delete;

    void join(const core::UserId& /*user*/, const core::DeviceId& /*device*/, MediaRole /*role*/,
              core::ports::TicketDone done) override {
        done_ = std::move(done);
        timer_ = reactor_.arm_timer(core::Millis{0}, handler_);
    }
    // The rest answer at once, which the port forbids but the wrapper does not care about.
    void participants(core::ports::ParticipantsDone done) override {
        done(std::vector<core::ports::MediaParticipant>{});
    }
    void relay(const core::UserId& /*user*/, const core::DeviceId& /*device*/,
               const core::ports::MediaRelay& target, core::ports::RelayDone done) override {
        done("relay-of-" + target.stream);
    }
    void close(core::ports::MediaDone done) override { done({}); }

private:
    struct Handler final : net::ITimerHandler {
        explicit Handler(FakeRoom& r) noexcept : room(r) {}
        void on_timeout() noexcept override {
            room.timer_.reset();
            if (room.refuse_) {
                room.done_(std::unexpected(*room.refuse_));
                return;
            }
            room.done_(MediaTicket{.endpoint = "wss://media.example.test",
                                   .credential = "ticket",
                                   .expires_at = core::WallTime{}});
        }
        FakeRoom& room;
    };

    net::IReactor& reactor_;
    std::optional<MediaError> refuse_;
    core::ports::TicketDone done_;
    Handler handler_{*this};
    std::optional<net::TimerId> timer_;
};

class FakeSfu final : public ISfu {
public:
    FakeSfu(net::IReactor& reactor, std::optional<MediaError> refuse,
            std::optional<MediaError> refuse_open = std::nullopt) noexcept
        : reactor_(reactor), refuse_(refuse), refuse_open_(refuse_open) {}
    // Opens synchronously, which the port forbids but the wrapper does not care about.
    void open_room(const core::RoomId& /*room*/, MediaGeneration /*generation*/,
                   MediaRoomKind /*kind*/, std::uint16_t /*max_participants*/,
                   OpenDone done) override {
        if (refuse_open_) {
            done(std::unexpected(*refuse_open_));
            return;
        }
        done(std::unique_ptr<IMediaRoom>(std::make_unique<FakeRoom>(reactor_, refuse_)));
    }
    void present(const core::RoomId& /*room*/, MediaGeneration /*generation*/,
                 const core::UserId& /*user*/, const core::DeviceId& /*device*/,
                 core::ports::PresenceDone done) override {
        done(true);
    }

private:
    net::IReactor& reactor_;
    std::optional<MediaError> refuse_;
    std::optional<MediaError> refuse_open_;
};

// Counts what it is asked for, and answers with one relay or a failure.
class CountingIssuer final : public ITurnCredentials {
public:
    explicit CountingIssuer(std::optional<MediaError> fail) noexcept : fail_(fail) {}
    void issue(const core::UserId& /*user*/, const core::DeviceId& device,
               core::ports::IceServersDone done) override {
        ++issued;
        if (fail_) {
            done(std::unexpected(*fail_));
            return;
        }
        done(std::vector<IceServer>{{.urls = {"turn:relay.example.com:3478"},
                                     .username = "1:" + device.to_string(),
                                     .credential = "c"}});
    }
    int issued = 0;

private:
    std::optional<MediaError> fail_;
};

class RelayedSfuTest : public TurnTest {
protected:
    TicketResult join(std::optional<MediaError> refuse, CountingIssuer& turn) {
        auto sfu = infra::turn::make_relayed_sfu(std::make_unique<FakeSfu>(*reactor, refuse), turn);
        std::unique_ptr<IMediaRoom> room;
        sfu->open_room(*core::RoomId::parse(kRoom), MediaGeneration{1}, MediaRoomKind::Call, 2,
                       [&](std::expected<std::unique_ptr<IMediaRoom>, MediaError> r) noexcept {
                           room = std::move(*r);
                       });
        EXPECT_TRUE(room);
        std::optional<TicketResult> got;
        room->join(*core::UserId::parse("caller"), *core::DeviceId::parse(kDevice),
                   MediaRole::Member, [&](TicketResult r) noexcept { got = std::move(r); });
        EXPECT_TRUE(pump_until(*reactor, [&] { return got.has_value(); }));
        return got.value_or(std::unexpected(MediaError::Unavailable));
    }
};

TEST_F(RelayedSfuTest, EveryTicketCarriesItsParticipantsRelays) {
    CountingIssuer turn(std::nullopt);
    const TicketResult ticket = join(std::nullopt, turn);
    ASSERT_TRUE(ticket);
    EXPECT_EQ(ticket->credential, "ticket");
    ASSERT_EQ(ticket->ice_servers.size(), 1U);
    EXPECT_EQ(ticket->ice_servers[0].username, "1:" + std::string(kDevice));
    EXPECT_EQ(turn.issued, 1);
}

TEST_F(RelayedSfuTest, ARefusedJoinIssuesNoCredential) {
    CountingIssuer turn(std::nullopt);
    const TicketResult ticket = join(MediaError::Refused, turn);
    ASSERT_FALSE(ticket);
    EXPECT_EQ(ticket.error(), MediaError::Refused);
    EXPECT_EQ(turn.issued, 0);
}

TEST_F(RelayedSfuTest, EverythingButJoinIsTheMediaServersOwn) {
    CountingIssuer turn(std::nullopt);
    auto sfu =
        infra::turn::make_relayed_sfu(std::make_unique<FakeSfu>(*reactor, std::nullopt), turn);
    std::optional<std::expected<bool, MediaError>> present;
    sfu->present(*core::RoomId::parse(kRoom), MediaGeneration{1}, *core::UserId::parse("caller"),
                 *core::DeviceId::parse(kDevice),
                 [&](std::expected<bool, MediaError> r) noexcept { present = r; });
    ASSERT_TRUE(present && *present);
    EXPECT_TRUE(**present);

    std::unique_ptr<IMediaRoom> room;
    sfu->open_room(*core::RoomId::parse(kRoom), MediaGeneration{1}, MediaRoomKind::Stream, 2,
                   [&](std::expected<std::unique_ptr<IMediaRoom>, MediaError> r) noexcept {
                       room = std::move(*r);
                   });
    ASSERT_TRUE(room);
    bool listed = false;
    room->participants(
        [&](std::expected<std::vector<core::ports::MediaParticipant>, MediaError> r) noexcept {
            listed = r.has_value();
        });
    EXPECT_TRUE(listed);
    std::optional<std::string> relay;
    room->relay(*core::UserId::parse("caller"), *core::DeviceId::parse(kDevice),
                core::ports::MediaRelay{
                    .stream = "s", .passphrase = "p", .keyframe_interval = core::Seconds{2}},
                [&](std::expected<std::string, MediaError> r) noexcept { relay = r.value_or(""); });
    EXPECT_EQ(relay, "relay-of-s");
    bool closed = false;
    room->close([&](std::expected<void, MediaError> r) noexcept { closed = r.has_value(); });
    EXPECT_TRUE(closed);
    EXPECT_EQ(turn.issued, 0) << "only a join issues credentials";
}

TEST_F(RelayedSfuTest, ARoomThatCannotOpenIsReportedAsItWas) {
    CountingIssuer turn(std::nullopt);
    auto sfu = infra::turn::make_relayed_sfu(
        std::make_unique<FakeSfu>(*reactor, std::nullopt, MediaError::Refused), turn);
    std::optional<MediaError> error;
    sfu->open_room(*core::RoomId::parse(kRoom), MediaGeneration{1}, MediaRoomKind::Call, 2,
                   [&](std::expected<std::unique_ptr<IMediaRoom>, MediaError> r) noexcept {
                       error = r ? std::nullopt : std::optional(r.error());
                   });
    EXPECT_EQ(error, MediaError::Refused);
}

TEST_F(RelayedSfuTest, ATicketWithoutItsRelaysIsNotIssued) {
    // A client given a ticket and no relay could not reach the media server at all.
    CountingIssuer turn(MediaError::Unavailable);
    const TicketResult ticket = join(std::nullopt, turn);
    ASSERT_FALSE(ticket);
    EXPECT_EQ(ticket.error(), MediaError::Unavailable);
}

} // namespace
