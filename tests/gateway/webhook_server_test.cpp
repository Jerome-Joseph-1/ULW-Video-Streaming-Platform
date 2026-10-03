#include "net/reactor_factory.hpp"
#include "net/socket.hpp"

#include "livekit_webhook.hpp"
#include "ops/log.hpp"
#include "publisher_watch.hpp"
#include "support/fake_clock.hpp"
#include "support/memory_log.hpp"
#include "support/reactor_harness.hpp"
#include "webhook_server.hpp"
#include "webhook_signer.hpp"

#include <sys/socket.h>

#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

namespace {

using gateway::WebhookEvent;
using gateway::WebhookEventKind;
using gateway::WebhookLimits;
using gateway::WebhookRejection;
using gateway::WebhookServer;
using ulw::test::FakeClock;
using ulw::test::kWebhookKey;
using ulw::test::kWebhookSecret;
using ulw::test::livekit_token;
using ulw::test::pump_pending;
using ulw::test::pump_until;
using ulw::test::sign_webhook;

constexpr std::string_view kBody =
    R"({"event":"participant_left","room":{"sid":"RM_1","name":"r:1"},)"
    R"("participant":{"sid":"PA_1","identity":"u/r"},"id":"EV_9","createdAt":"1767225600"})";

struct RecordingSink final : gateway::IWebhookSink {
    void on_event(const WebhookEvent& event) noexcept override { events.push_back(event); }
    std::vector<WebhookEvent> events;
};

std::string post(std::string_view body, std::string_view authorization,
                 std::string_view path = gateway::kWebhookPath, std::string_view method = "POST",
                 std::string_view extra = "") {
    std::string r = std::string(method) + " " + std::string(path) + " HTTP/1.1\r\n";
    r += "Host: video-gateway-hooks\r\nContent-Type: application/webhook+json\r\n";
    r += "User-Agent: LiveKit\r\n";
    if (!authorization.empty()) {
        r += "Authorization: " + std::string(authorization) + "\r\n";
    }
    r += extra;
    r += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    r += body;
    return r;
}

class WebhookServerTest : public ::testing::Test {
protected:
    void SetUp() override { start(WebhookLimits{}); }

    void TearDown() override {
        clients.clear();
        server.reset();
        reactor.reset();
    }

    void start(const WebhookLimits& limits) {
        clients.clear();
        server.reset();
        reactor.reset();
        auto r = net::make_reactor(net::ReactorKind::Epoll, clock, 256);
        ASSERT_TRUE(r);
        reactor = std::move(*r);
        server = std::make_unique<WebhookServer>(
            *reactor, clock,
            gateway::WebhookKey{.id = std::string(kWebhookKey),
                                .secret = std::string(kWebhookSecret)},
            sink, log, limits);
        auto listener = net::listen_tcp({.port = 0, .loopback_only = true});
        ASSERT_TRUE(listener);
        port = *net::local_port(listener->get());
        ASSERT_TRUE(reactor->listen(std::move(*listener), *server));
    }

    int connect() {
        clients.push_back(ulw::test::connect_loopback(port));
        EXPECT_TRUE(clients.back());
        return clients.back().get();
    }

    // Sends `bytes` and returns once `responses` answers have come back whole (none has a
    // body), or the peer has closed.
    std::string exchange(int fd, std::string_view bytes, std::size_t responses = 1) {
        std::size_t sent = 0;
        std::string got;
        bool closed = false;
        const bool done = pump_until(*reactor, [&] {
            if (sent < bytes.size()) {
                sent += ulw::test::write_some(fd, std::as_bytes(std::span(bytes.substr(sent))));
            }
            closed = read_into(fd, got) || closed;
            std::size_t ends = 0;
            for (auto at = got.find("\r\n\r\n"); at != std::string::npos;
                 at = got.find("\r\n\r\n", at + 4)) {
                ++ends;
            }
            return ends >= responses || closed;
        });
        EXPECT_TRUE(done);
        server->reap();
        return got;
    }

    // Appends what is there; true when the peer has closed.
    static bool read_into(int fd, std::string& got) {
        std::array<char, 4096> buf{};
        for (;;) {
            const ssize_t n = ::recv(fd, buf.data(), buf.size(), MSG_DONTWAIT);
            if (n == 0) {
                return true;
            }
            if (n < 0) {
                return false;
            }
            got.append(buf.data(), static_cast<std::size_t>(n));
        }
    }

    bool peer_closed(int fd) {
        std::string ignored;
        return pump_until(*reactor, [&] { return read_into(fd, ignored); });
    }

    std::int64_t now() const {
        return std::chrono::floor<std::chrono::seconds>(clock.wall_now().time_since_epoch())
            .count();
    }

    std::size_t refused(WebhookRejection r) const {
        return server->counters().refused.at(static_cast<std::size_t>(r));
    }

    FakeClock clock;
    ulw::test::MemoryLog log_sink;
    ops::Logger log{log_sink, clock, "gateway", ops::Level::Debug};
    RecordingSink sink;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<WebhookServer> server;
    std::uint16_t port = 0;
    std::vector<os::UniqueFd> clients;
};

TEST_F(WebhookServerTest, AVerifiedEventIsAnsweredAndHandedOn) {
    const int fd = connect();
    const std::string answer = exchange(fd, post(kBody, livekit_token(kBody, now())));
    EXPECT_TRUE(answer.starts_with("HTTP/1.1 200 ")) << answer;
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].kind, WebhookEventKind::ParticipantLeft);
    EXPECT_EQ(sink.events[0].room, "r:1");
    EXPECT_EQ(sink.events[0].identity, "u/r");
    EXPECT_EQ(server->counters().accepted, 1U);
}

TEST_F(WebhookServerTest, OneConnectionCarriesEventAfterEvent) {
    const int fd = connect();
    const std::string two =
        post(kBody, livekit_token(kBody, now())) + post(kBody, livekit_token(kBody, now()));
    const std::string answer = exchange(fd, two, 2);
    EXPECT_EQ(answer.find("HTTP/1.1 200 "), 0U) << answer;
    EXPECT_NE(answer.find("HTTP/1.1 200 ", 1), std::string::npos) << answer;
    EXPECT_EQ(sink.events.size(), 2U);
    EXPECT_FALSE(answer.contains("Connection: close")) << answer;
}

TEST_F(WebhookServerTest, NothingUnverifiedIsHandedOn) {
    struct Case {
        std::string authorization;
        std::string body;
        WebhookRejection reason;
    };
    const std::string body(kBody);
    const std::vector<Case> cases{
        {"", body, WebhookRejection::NoAuthorization},
        {sign_webhook(body, {.secret = "fake-another-secret-0123456789abcdef01",
                             .issued = now(),
                             .expires = now() + 300}),
         body, WebhookRejection::Signature},
        {livekit_token(body, now()), body + " ", WebhookRejection::BodyHash},
        {livekit_token(body, now() - 3600), body, WebhookRejection::Expired},
        {"Bearer " + livekit_token(body, now()), body, WebhookRejection::Malformed},
    };
    for (const Case& c : cases) {
        const int fd = connect();
        const std::string answer = exchange(fd, post(c.body, c.authorization));
        EXPECT_TRUE(answer.starts_with("HTTP/1.1 401 ")) << to_string(c.reason) << ": " << answer;
        EXPECT_TRUE(answer.contains("Connection: close")) << answer;
        EXPECT_TRUE(peer_closed(fd));
        EXPECT_EQ(refused(c.reason), 1U) << to_string(c.reason);
    }
    EXPECT_TRUE(sink.events.empty());
    EXPECT_EQ(server->counters().accepted, 0U);
    EXPECT_EQ(log_sink.events("live webhook refused").size(), cases.size());
}

TEST_F(WebhookServerTest, AnOversizedBodyIsRefusedBeforeItIsRead) {
    const int fd = connect();
    // Only the head is sent: the answer comes without waiting for 64 KiB more.
    std::string head = post("", livekit_token("", now()));
    head.replace(head.find("Content-Length: 0"), 17,
                 "Content-Length: " + std::to_string(gateway::kMaxWebhookBody + 1));
    const std::string answer = exchange(fd, head);
    EXPECT_TRUE(answer.starts_with("HTTP/1.1 413 ")) << answer;
    EXPECT_EQ(refused(WebhookRejection::TooLarge), 1U);
    EXPECT_TRUE(sink.events.empty());
}

TEST_F(WebhookServerTest, TheLargestBodyIsTaken) {
    std::string body(kBody);
    body.insert(body.size() - 1, R"(,"pad":")" +
                                     std::string(gateway::kMaxWebhookBody - body.size() - 9, 'x') +
                                     R"(")");
    ASSERT_EQ(body.size(), gateway::kMaxWebhookBody);
    const int fd = connect();
    const std::string answer = exchange(fd, post(body, livekit_token(body, now())));
    EXPECT_TRUE(answer.starts_with("HTTP/1.1 200 ")) << answer;
    EXPECT_EQ(sink.events.size(), 1U);
}

TEST_F(WebhookServerTest, OnlyItsOnePathAndMethodAreServed) {
    const std::string token = livekit_token(kBody, now());
    EXPECT_TRUE(
        exchange(connect(), post(kBody, token, "/api/v1/live")).starts_with("HTTP/1.1 404"));
    EXPECT_TRUE(exchange(connect(), post(kBody, token, "/livekit/webhook?x=1"))
                    .starts_with("HTTP/1.1 404"));
    EXPECT_TRUE(exchange(connect(), post("", token, gateway::kWebhookPath, "GET"))
                    .starts_with("HTTP/1.1 405"));
    EXPECT_TRUE(exchange(connect(), "NOT HTTP\r\n\r\n").starts_with("HTTP/1.1 400"));
    EXPECT_TRUE(sink.events.empty());
    EXPECT_EQ(server->counters().bad_requests, 4U);
}

TEST_F(WebhookServerTest, AVerifiedBodyThatIsNoEventIsABadRequest) {
    const std::string body = "[1,2,3]";
    const std::string answer = exchange(connect(), post(body, livekit_token(body, now())));
    EXPECT_TRUE(answer.starts_with("HTTP/1.1 400 ")) << answer;
    EXPECT_TRUE(sink.events.empty());
}

TEST_F(WebhookServerTest, RequestsPastTheRateAreToldToComeBack) {
    start(WebhookLimits{.rate = {.burst = 2, .per_second = 1}});
    const std::string one = post(kBody, livekit_token(kBody, now()));
    EXPECT_TRUE(exchange(connect(), one).starts_with("HTTP/1.1 200"));
    EXPECT_TRUE(exchange(connect(), one).starts_with("HTTP/1.1 200"));
    const std::string limited = exchange(connect(), one);
    EXPECT_TRUE(limited.starts_with("HTTP/1.1 429 ")) << limited;
    EXPECT_TRUE(limited.contains("Retry-After: 1")) << limited;
    EXPECT_EQ(server->counters().limited, 1U);
    EXPECT_EQ(sink.events.size(), 2U);
    clock.advance(core::Millis{1'000});
    EXPECT_TRUE(exchange(connect(), one).starts_with("HTTP/1.1 200"));
}

TEST_F(WebhookServerTest, ConnectionsPastTheCapAreClosed) {
    start(WebhookLimits{.max_connections = 2});
    const int a = connect();
    const int b = connect();
    ASSERT_TRUE(pump_until(*reactor, [&] { return server->connections() == 2U; }));
    const int c = connect();
    EXPECT_TRUE(peer_closed(c));
    EXPECT_EQ(server->counters().refused_connections, 1U);
    // The two held still serve.
    EXPECT_TRUE(exchange(a, post(kBody, livekit_token(kBody, now()))).starts_with("HTTP/1.1 200"));
    EXPECT_TRUE(exchange(b, post(kBody, livekit_token(kBody, now()))).starts_with("HTTP/1.1 200"));
}

TEST_F(WebhookServerTest, AnIdleOrStalledConnectionIsClosed) {
    start(WebhookLimits{.idle_timeout = core::Millis{5'000}});
    const int idle = connect();
    const int stalled = connect();
    ASSERT_TRUE(pump_until(*reactor, [&] { return server->connections() == 2U; }));
    const std::string half = post(kBody, livekit_token(kBody, now())).substr(0, 40);
    ASSERT_EQ(ulw::test::write_some(stalled, std::as_bytes(std::span(half))), half.size());
    pump_pending(*reactor);
    clock.advance(core::Millis{5'000});
    EXPECT_TRUE(peer_closed(idle));
    EXPECT_TRUE(peer_closed(stalled));
    ASSERT_TRUE(pump_until(*reactor, [&] {
        server->reap();
        return server->connections() == 0U;
    }));
}

} // namespace
