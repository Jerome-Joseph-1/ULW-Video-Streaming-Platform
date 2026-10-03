#include "infra/curl/multi.hpp"
#include "infra/webpush/curl_transport.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"

#include "support/https_test_server.hpp"
#include "support/reactor_harness.hpp"
#include "support/tls_pki.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>

namespace {

using namespace infra::webpush;
using ulw::test::HttpsTestServer;
using ulw::test::pump_until;
using ulw::test::Reply;
using ulw::test::ServedRequest;

class CurlTransportTest : public ::testing::Test {
protected:
    CurlTransportTest() {
        auto r = net::make_reactor(net::ReactorKind::Epoll, clock_, 1024);
        EXPECT_TRUE(r.has_value());
        reactor_ = std::move(*r);
        auto m = infra::curl::Multi::create(*reactor_, 4);
        EXPECT_TRUE(m.has_value());
        multi_ = std::move(*m);
    }

    // The transport as chat_server makes it, but for a push service on loopback with its own CA.
    std::unique_ptr<CurlPushTransport> transport(bool public_only) {
        return std::make_unique<CurlPushTransport>(
            *multi_, CurlTransportOptions{.timeout = std::chrono::seconds(10),
                                          .public_only = public_only,
                                          .ca_file = ulw::test::TestPki::shared().ca_file()});
    }

    std::optional<PushOutcome> post(CurlPushTransport& t, std::string url) {
        std::optional<PushOutcome> got;
        const bool started =
            t.post(PushRequest{.url = std::move(url),
                               .headers = {"TTL: 30", "Content-Encoding: aes128gcm"},
                               .body = {0xde, 0xad, 0xbe, 0xef}},
                   [&got](PushOutcome o) noexcept { got = o; });
        EXPECT_TRUE(started);
        EXPECT_TRUE(pump_until(*reactor_, [&] { return got.has_value(); }));
        return got;
    }

    os::SystemClock clock_;
    std::unique_ptr<net::IReactor> reactor_;
    std::unique_ptr<infra::curl::Multi> multi_;
};

TEST_F(CurlTransportTest, PostsTheBodyAndHeadersOverTls) {
    const HttpsTestServer server([](const ServedRequest&) {
        return Reply{.status = 201, .headers = {{"Location", "/m/1"}}, .body = {}};
    });
    auto t = transport(false);
    const auto got = post(*t, server.base_url() + "/push/abc");
    ASSERT_TRUE(got && got->has_value());
    EXPECT_EQ((*got)->status, 201);
    EXPECT_FALSE((*got)->retry_after.has_value());
    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests[0].method, "POST");
    EXPECT_EQ(requests[0].target, "/push/abc");
    EXPECT_EQ(requests[0].header("ttl"), "30");
    EXPECT_EQ(requests[0].header("content-encoding"), "aes128gcm");
    EXPECT_EQ(requests[0].body, std::string("\xde\xad\xbe\xef", 4));
    EXPECT_EQ(t->running(), 0U);
}

TEST_F(CurlTransportTest, ReadsRetryAfterInSeconds) {
    const HttpsTestServer server([](const ServedRequest&) {
        return Reply{.status = 429, .headers = {{"Retry-After", "7"}}, .body = "slow down"};
    });
    auto t = transport(false);
    const auto got = post(*t, server.base_url() + "/push/abc");
    ASSERT_TRUE(got && got->has_value());
    EXPECT_EQ((*got)->status, 429);
    EXPECT_EQ((*got)->retry_after, core::Seconds{7});
}

TEST_F(CurlTransportTest, RefusesALoopbackPushServiceWithoutConnecting) {
    const HttpsTestServer server(
        [](const ServedRequest&) { return Reply{.status = 201, .headers = {}, .body = {}}; });
    auto t = transport(true);
    const auto got = post(*t, server.base_url() + "/push/abc");
    ASSERT_TRUE(got.has_value());
    ASSERT_FALSE(got->has_value());
    EXPECT_EQ(got->error(), PushFailure::AddressRefused);
    EXPECT_EQ(server.connections(), 0U);
    // By its address too, which the endpoint check would have refused first.
    const auto literal =
        post(*t, "https://127.0.0.1:" + std::to_string(server.port()) + "/push/abc");
    ASSERT_TRUE(literal && !literal->has_value());
    EXPECT_EQ(literal->error(), PushFailure::AddressRefused);
    EXPECT_EQ(server.connections(), 0U);
}

TEST_F(CurlTransportTest, RefusesPlainHttp) {
    auto t = transport(false);
    const auto got = post(*t, "http://localhost:1/push");
    ASSERT_TRUE(got && !got->has_value());
    EXPECT_EQ(got->error(), PushFailure::Local);
}

TEST_F(CurlTransportTest, AnUntrustedCertificateIsATlsFailure) {
    const HttpsTestServer server(
        [](const ServedRequest&) { return Reply{.status = 201, .headers = {}, .body = {}}; });
    CurlPushTransport t(*multi_, CurlTransportOptions{.public_only = false, .ca_file = {}});
    const auto got = post(t, server.base_url() + "/push/abc");
    ASSERT_TRUE(got && !got->has_value());
    EXPECT_EQ(got->error(), PushFailure::Tls);
}

TEST_F(CurlTransportTest, APostRunningWhenTheTransportGoesNeverAnswers) {
    const HttpsTestServer server(
        [](const ServedRequest&) { return Reply{.status = 201, .headers = {}, .body = {}}; });
    auto t = transport(false);
    bool called = false;
    ASSERT_TRUE(t->post(PushRequest{.url = server.base_url() + "/x", .headers = {}, .body = {1}},
                        [&called](PushOutcome) noexcept { called = true; }));
    EXPECT_EQ(t->running(), 1U);
    t.reset();
    ulw::test::pump_pending(*reactor_);
    EXPECT_FALSE(called);
}

TEST(OutcomeOf, MapsFailuresAndReadsOnlyDeltaSeconds) {
    using infra::curl::Failure;
    using infra::curl::FailureKind;
    const auto kind = [](FailureKind k) {
        return outcome_of(std::unexpected(Failure{.kind = k, .detail = {}})).error();
    };
    EXPECT_EQ(kind(FailureKind::AddressRefused), PushFailure::AddressRefused);
    EXPECT_EQ(kind(FailureKind::Tls), PushFailure::Tls);
    EXPECT_EQ(kind(FailureKind::Local), PushFailure::Local);
    for (const FailureKind k : {FailureKind::Resolve, FailureKind::Connect, FailureKind::Timeout,
                                FailureKind::Network, FailureKind::BodyTooLarge}) {
        EXPECT_EQ(kind(k), PushFailure::Network);
    }
    const auto with = [](std::string headers) {
        return outcome_of(
                   infra::curl::Response{.status = 503, .headers = std::move(headers), .body = {}})
            ->retry_after;
    };
    EXPECT_EQ(with("HTTP/1.1 503 x\r\nRetry-After: 120\r\n\r\n"), core::Seconds{120});
    EXPECT_FALSE(with("HTTP/1.1 503 x\r\nRetry-After: Wed, 21 Oct 2026 07:28:00 GMT\r\n\r\n"));
    EXPECT_FALSE(with("HTTP/1.1 503 x\r\nRetry-After: -1\r\n\r\n"));
    EXPECT_FALSE(with("HTTP/1.1 503 x\r\nRetry-After: 86401\r\n\r\n"));
    EXPECT_FALSE(with("HTTP/1.1 503 x\r\n\r\n"));
}

} // namespace
