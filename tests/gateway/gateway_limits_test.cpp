#include "core/util/json.hpp"

#include "gateway_harness.hpp"
#include "support/eventually.hpp"
#include "support/http_client.hpp"
#include "support/reactor_harness.hpp"

#include <chrono>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <string_view>

namespace {

using namespace std::chrono_literals;
using ulw::test::Backend;
using ulw::test::GatewayOptions;
using ulw::test::GatewayUnderTest;
using ulw::test::HttpClient;
using ulw::test::HttpResponse;
using ulw::test::kMiB;

constexpr std::string_view kAlice = "user.alice";
constexpr std::string_view kBob = "user.bob";
// Nobody's video: an authenticated lookup that answers 404 once it reaches the catalog.
const std::string kNoVideo = "/api/v1/videos/01890a5d-ac96-774b-bcce-b302099a8057";

std::optional<HttpResponse> healthz(const GatewayUnderTest& gw,
                                    std::string_view forwarded_for = {}) {
    HttpClient c(gw.endpoint());
    if (forwarded_for.empty()) {
        return c.request("GET", "/api/v1/healthz", "");
    }
    return c.request("GET", "/api/v1/healthz", "", {},
                     {{"X-Forwarded-For", std::string(forwarded_for)}});
}

std::optional<std::string> create_upload(HttpClient& c, std::uint64_t size,
                                         std::string_view token) {
    const std::string body = R"({"filename":"trip.mp4","size_bytes":)" + std::to_string(size) +
                             R"(,"content_type":"video/mp4"})";
    const auto r = c.request("POST", "/api/v1/uploads", token, std::as_bytes(std::span(body)));
    if (!r || r->status != 201) {
        return std::nullopt;
    }
    const auto doc = core::json::parse(r->body);
    return doc ? std::optional(std::string(*doc->find("upload_id")->as_string())) : std::nullopt;
}

std::optional<HttpResponse> patch(HttpClient& c, const std::string& upload, std::uint64_t offset,
                                  std::uint64_t bytes, std::string_view token) {
    return c.request("PATCH", "/api/v1/uploads/" + upload, token,
                     ulw::test::pattern(static_cast<std::size_t>(bytes)),
                     {{"Upload-Offset", std::to_string(offset)}});
}

TEST(GatewayClientLimits, AnAddressAtItsConnectionLimitIsResetUntilOneCloses) {
    GatewayOptions options;
    options.limits.max_connections_per_ip = 2;
    GatewayUnderTest gw(options);
    std::optional<HttpClient> first(gw.endpoint());
    HttpClient second(gw.endpoint());
    ASSERT_EQ(first->request("GET", "/api/v1/healthz", "")->status, 200);
    ASSERT_EQ(second.request("GET", "/api/v1/healthz", "")->status, 200);
    // The kernel completes the handshake; the gateway resets it before reading a byte.
    EXPECT_FALSE(healthz(gw));
    EXPECT_EQ(gw.counters().rejected_ip_connections, 1U);
    first.reset();
    ASSERT_TRUE(ulw::test::eventually([&] { return gw.connections() == 1; }));
    const auto after = healthz(gw);
    ASSERT_TRUE(after);
    EXPECT_EQ(after->status, 200);
}

TEST(GatewayClientLimits, NewConnectionsFasterThanTheRateAreResetUntilTheBucketRefills) {
    GatewayOptions options{.manual_clock = true};
    options.limits.new_connections_per_ip_per_second = 2;
    GatewayUnderTest gw(options);
    EXPECT_EQ(healthz(gw)->status, 200);
    EXPECT_EQ(healthz(gw)->status, 200);
    EXPECT_FALSE(healthz(gw));
    EXPECT_EQ(gw.counters().rejected_ip_rate, 1U);
    // Two a second: half a second brings one back, and only one.
    gw.advance(500ms);
    EXPECT_EQ(healthz(gw)->status, 200);
    EXPECT_FALSE(healthz(gw));
    EXPECT_EQ(gw.counters().rejected_ip_rate, 2U);
}

TEST(GatewayClientLimits, AnUntrustedPeersForwardedForChangesNothing) {
    GatewayOptions options;
    options.limits.max_connections_per_ip = 1;
    const GatewayUnderTest gw(options);
    HttpClient held(gw.endpoint());
    ASSERT_EQ(held.request("GET", "/api/v1/healthz", "")->status, 200);
    EXPECT_FALSE(healthz(gw, "198.51.100.9"));
}

class GatewayBehindProxy : public ::testing::Test {
protected:
    static GatewayOptions options() {
        GatewayOptions o;
        o.limits.max_connections_per_ip = 1;
        o.limits.trusted_proxies = {*net::IpNetwork::parse("127.0.0.1/32")};
        return o;
    }
};

TEST_F(GatewayBehindProxy, ClientsAreCountedByTheAddressTheProxyNames) {
    GatewayUnderTest gw(options());
    // A request for 198.51.100.1 held in flight, waiting on a signing key.
    HttpClient pending(gw.endpoint());
    ASSERT_TRUE(pending.send_request("GET", kNoVideo, "slow.alice", {},
                                     {{"X-Forwarded-For", "198.51.100.1"}}));
    ASSERT_TRUE(ulw::test::eventually([&] { return gw.key_waiters() == 1; }));

    // The proxy's own connections are not held to the per-address limit of 1.
    const auto same = healthz(gw, "198.51.100.1");
    ASSERT_TRUE(same);
    EXPECT_EQ(same->status, 429);
    EXPECT_EQ(same->header("retry-after"), "1");
    EXPECT_EQ(healthz(gw, "198.51.100.2")->status, 200);
    // A client naming someone else in front of the proxy's own entry is still itself.
    EXPECT_EQ(healthz(gw, "198.51.100.2, 198.51.100.1")->status, 429);
    EXPECT_EQ(gw.counters().limited_ip_requests, 2U);

    gw.refresh_keys();
    const auto finished = pending.read_response();
    ASSERT_TRUE(finished);
    EXPECT_EQ(finished->status, 404);
    EXPECT_EQ(healthz(gw, "198.51.100.1")->status, 200);
}

TEST(GatewayUserLimits, AUserPastTheRequestRateGets429WithTheWaitInRetryAfter) {
    GatewayOptions options{.manual_clock = true};
    options.limits.requests_per_user_per_minute = 2;
    GatewayUnderTest gw(options);
    HttpClient alice(gw.endpoint());
    EXPECT_EQ(alice.request("GET", kNoVideo, kAlice)->status, 404);
    EXPECT_EQ(alice.request("GET", kNoVideo, kAlice)->status, 404);
    const auto refused = alice.request("GET", kNoVideo, kAlice);
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->status, 429);
    // Two a minute: the next one in 30 s.
    EXPECT_EQ(refused->header("retry-after"), "30");
    EXPECT_EQ(gw.counters().limited_user_requests, 1U);

    HttpClient bob(gw.endpoint());
    EXPECT_EQ(bob.request("GET", kNoVideo, kBob)->status, 404);
    // Unauthenticated requests are nobody's to count.
    EXPECT_EQ(bob.request("GET", "/api/v1/healthz", "")->status, 200);

    gw.advance(30s);
    HttpClient again(gw.endpoint());
    EXPECT_EQ(again.request("GET", kNoVideo, kAlice)->status, 404);
    EXPECT_EQ(again.request("GET", kNoVideo, kAlice)->status, 429);
}

TEST(GatewayUserLimits, APatchPastTheDailyByteQuotaGets429AndTakesNoSlot) {
    GatewayOptions options{.backend = Backend::Fake, .chunk = kMiB};
    options.limits.upload_bytes_per_user_per_day = 2 * kMiB;
    options.limits.max_uploads_per_user = 1;
    GatewayUnderTest gw(options);
    HttpClient alice(gw.endpoint());
    const auto up = create_upload(alice, 3 * kMiB, kAlice);
    ASSERT_TRUE(up);
    EXPECT_EQ(patch(alice, *up, 0, kMiB, kAlice)->status, 204);
    EXPECT_EQ(patch(alice, *up, kMiB, kMiB, kAlice)->status, 204);
    HttpClient refused_client(gw.endpoint());
    const auto refused = patch(refused_client, *up, 2 * kMiB, kMiB, kAlice);
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->status, 429);
    // 1 MiB short at 2 MiB a day: 12 hours.
    EXPECT_EQ(refused->header("retry-after"), "43200");
    EXPECT_EQ(gw.counters().limited_user_bytes, 1U);

    // The refusal gave back the one upload slot alice may hold, and bob has a quota of his own.
    HttpClient bob(gw.endpoint());
    const auto his = create_upload(bob, kMiB, kBob);
    ASSERT_TRUE(his);
    EXPECT_EQ(patch(bob, *his, 0, kMiB, kBob)->status, 204);
    HttpClient probe(gw.endpoint());
    const auto metrics = probe.request("GET", "/metrics", "");
    ASSERT_TRUE(metrics);
    EXPECT_NE(metrics->body.find("\nuploads_in_flight 0\n"), std::string::npos);
}

} // namespace
