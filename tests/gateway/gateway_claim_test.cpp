// Upload claims (ADR-0065) belong to the request that took them: each request gives its own
// back however it ends, and catalog_claims_held, the gateway's count of them, comes back to 0.

#include "core/util/json.hpp"
#include "core/util/parse.hpp"

#include "gateway_harness.hpp"
#include "support/eventually.hpp"
#include "support/http_client.hpp"
#include "support/reactor_harness.hpp"

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace {

using ulw::test::Backend;
using ulw::test::GatewayOptions;
using ulw::test::GatewayUnderTest;
using ulw::test::HttpClient;
using ulw::test::kMiB;

constexpr std::string_view kAlice = "user.alice";
constexpr std::string_view kVideo = "01890a5d-ac96-774b-bcce-b302099a8057";
constexpr std::string_view kMaster = "#EXTM3U\n"
                                     "#EXT-X-VERSION:7\n"
                                     "#EXT-X-STREAM-INF:BANDWIDTH=1020800,RESOLUTION=1280x720,"
                                     "CODECS=\"avc1.4d401f,mp4a.40.2\"\n"
                                     "720p/index.m3u8\n";

std::optional<std::string> create_upload(HttpClient& c, std::uint64_t size) {
    const std::string body = R"({"filename":"trip.mp4","size_bytes":)" + std::to_string(size) +
                             R"(,"content_type":"video/mp4"})";
    const auto r = c.request("POST", "/api/v1/uploads", kAlice, std::as_bytes(std::span(body)));
    if (!r || r->status != 201) {
        return std::nullopt;
    }
    const auto doc = core::json::parse(r->body);
    if (!doc) {
        return std::nullopt;
    }
    return std::string(*doc->find("upload_id")->as_string());
}

std::string patch_head(const std::string& upload, std::uint64_t offset, std::uint64_t length) {
    return "PATCH /api/v1/uploads/" + upload +
           " HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer user.alice\r\n"
           "Upload-Offset: " +
           std::to_string(offset) + "\r\nContent-Length: " + std::to_string(length) + "\r\n\r\n";
}

std::optional<ulw::test::HttpResponse> patch(HttpClient& c, const std::string& upload,
                                             std::uint64_t offset,
                                             std::span<const std::byte> bytes) {
    return c.request("PATCH", "/api/v1/uploads/" + upload, kAlice, bytes,
                     {{"Upload-Offset", std::to_string(offset)}});
}

// catalog_claims_held as /metrics reports it.
std::optional<std::uint64_t> scraped_claims(GatewayUnderTest& gw) {
    const std::string text = gw.metrics();
    constexpr std::string_view kSeries = "\ncatalog_claims_held ";
    const auto at = text.find(kSeries);
    if (at == std::string::npos) {
        return std::nullopt;
    }
    const std::string_view rest = std::string_view(text).substr(at + kSeries.size());
    const std::size_t end = rest.find('\n');
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    return core::parse_integer<std::uint64_t>(rest.substr(0, end));
}

// The catalog's own claims, the gateway's count and the scraped gauge all agree on `n`.
bool claims_are(GatewayUnderTest& gw, std::size_t n) {
    return gw.claims() == n && gw.claims_held() == n && scraped_claims(gw) == n;
}

GatewayOptions fake_store(bool manual_clock = false) {
    return GatewayOptions{.backend = Backend::Fake, .chunk = kMiB, .manual_clock = manual_clock};
}

TEST(GatewayClaims, TheGaugeCountsAChunkInFlightAndIsZeroOnceItCompletes) {
    GatewayUnderTest gw(fake_store());
    EXPECT_TRUE(claims_are(gw, 0));
    const auto data = ulw::test::pattern(kMiB);
    HttpClient c(gw.endpoint());
    const auto up = create_upload(c, kMiB);
    ASSERT_TRUE(up);
    ASSERT_TRUE(c.send_raw(patch_head(*up, 0, kMiB)));
    ASSERT_TRUE(c.send_raw(std::span(data).first(1000)));
    ASSERT_TRUE(ulw::test::eventually([&] { return gw.claims_held() == 1; }));
    EXPECT_TRUE(claims_are(gw, 1));
    ASSERT_TRUE(c.send_raw(std::span(data).subspan(1000)));
    const auto r = c.read_response();
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 204);
    // Given back in the same turn of the loop that sent the response.
    EXPECT_TRUE(claims_are(gw, 0));
}

// A keep-alive response ends its request with the claim still held: nothing on this path
// gives it back but the request finishing. The next PATCH on the same connection must find
// the upload free, not refused as claimed by itself.
TEST(GatewayClaims, AKeepAliveRequestThatEndsHoldingItsClaimGivesItBack) {
    GatewayUnderTest gw(fake_store());
    const auto data = ulw::test::pattern(kMiB);
    HttpClient c(gw.endpoint());
    const auto up = create_upload(c, kMiB);
    ASSERT_TRUE(up);
    const auto empty = patch(c, *up, 0, {});
    ASSERT_TRUE(empty);
    EXPECT_EQ(empty->status, 204);
    EXPECT_TRUE(claims_are(gw, 0));
    const auto full = patch(c, *up, 0, data);
    ASSERT_TRUE(full);
    EXPECT_EQ(full->status, 204);
    EXPECT_EQ(full->upload_offset(), kMiB);
    EXPECT_TRUE(claims_are(gw, 0));
}

TEST(GatewayClaims, TheGaugeReturnsToZeroAfterEachKindOfRefusal) {
    GatewayOptions options = fake_store();
    options.plan.fail_chunk = 1;
    GatewayUnderTest gw(options);
    const auto data = ulw::test::pattern(3 * kMiB);
    {
        // A chunk longer than what is left of the upload.
        HttpClient c(gw.endpoint());
        const auto up = create_upload(c, kMiB);
        ASSERT_TRUE(up);
        const auto r = patch(c, *up, 0, std::span(data).first(2 * kMiB));
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 400);
        EXPECT_TRUE(ulw::test::eventually([&] { return claims_are(gw, 0); }));
    }
    {
        // An offset neither the catalog nor the store agrees with: 409 after asking the store.
        HttpClient c(gw.endpoint());
        const auto up = create_upload(c, 3 * kMiB);
        ASSERT_TRUE(up);
        const auto r = patch(c, *up, 2 * kMiB, std::span(data).first(kMiB));
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 409);
        EXPECT_EQ(r->upload_offset(), 0U);
        EXPECT_TRUE(ulw::test::eventually([&] { return claims_are(gw, 0); }));
    }
    {
        // The store fails the chunk.
        HttpClient c(gw.endpoint());
        const auto up = create_upload(c, kMiB);
        ASSERT_TRUE(up);
        const auto r = patch(c, *up, 0, std::span(data).first(kMiB));
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 500);
        EXPECT_TRUE(ulw::test::eventually([&] { return claims_are(gw, 0); }));
    }
}

TEST(GatewayClaims, TheGaugeReturnsToZeroWhenTheBodyTimesOut) {
    GatewayUnderTest gw(fake_store(true));
    HttpClient c(gw.endpoint());
    const auto up = create_upload(c, kMiB);
    ASSERT_TRUE(up);
    ASSERT_TRUE(c.send_raw(patch_head(*up, 0, 1000) + "abc"));
    ASSERT_TRUE(ulw::test::eventually([&] { return claims_are(gw, 1); }));
    gw.advance(gateway::Limits{}.body_idle_timeout);
    const auto r = c.read_response();
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 408);
    EXPECT_TRUE(ulw::test::eventually([&] { return claims_are(gw, 0); }));
}

TEST(GatewayClaims, TheGaugeReturnsToZeroWhenTheClientAbandonsTheChunk) {
    GatewayUnderTest gw(fake_store());
    const auto data = ulw::test::pattern(kMiB);
    std::string upload;
    {
        HttpClient c(gw.endpoint());
        const auto up = create_upload(c, kMiB);
        ASSERT_TRUE(up);
        upload = *up;
        ASSERT_TRUE(c.send_raw(patch_head(upload, 0, kMiB)));
        ASSERT_TRUE(c.send_raw(std::span(data).first(1000)));
        ASSERT_TRUE(ulw::test::eventually([&] { return claims_are(gw, 1); }));
    }
    EXPECT_TRUE(ulw::test::eventually([&] { return claims_are(gw, 0); }));
    HttpClient again(gw.endpoint());
    const auto retried = patch(again, upload, 0, data);
    ASSERT_TRUE(retried);
    EXPECT_EQ(retried->status, 204);
    EXPECT_TRUE(claims_are(gw, 0));
}

// A gauge and regression test, not a reproduction: the catalog's answer to a claim reaches a
// request that has already ended, and the claim it grants goes straight back and is never
// counted as held. The old code released it too, through the ended request; what decides
// between old and new ownership is UploadClaim.AStaleCompletionCannotReleaseALaterRequestsClaim
// in upload_claim_test.cpp.
TEST(GatewayClaims, AClaimGrantedAfterItsRequestEndedIsGivenBackAndNeverCounted) {
    GatewayUnderTest gw(fake_store(true));
    const auto data = ulw::test::pattern(kMiB);
    HttpClient setup(gw.endpoint());
    const auto up = create_upload(setup, kMiB);
    ASSERT_TRUE(up);
    gw.hold_claims(true);
    HttpClient c(gw.endpoint());
    ASSERT_TRUE(c.send_raw(patch_head(*up, 0, kMiB)));
    ASSERT_TRUE(ulw::test::eventually([&] { return gw.held_claims() == 1; }));
    // Taken in the catalog, not yet answered: the gateway does not count it.
    EXPECT_EQ(gw.claims(), 1U);
    EXPECT_EQ(gw.claims_held(), 0U);
    gw.advance(gateway::Limits{}.request_backstop);
    EXPECT_TRUE(c.closed_by_peer());
    EXPECT_EQ(gw.counters().timeouts_backstop, 1U);
    gw.hold_claims(false);
    // The idle setup connection went at its header timeout in the same stretch of time.
    EXPECT_TRUE(ulw::test::eventually([&] { return claims_are(gw, 0) && gw.connections() == 0; }));
    HttpClient again(gw.endpoint());
    const auto retried = patch(again, *up, 0, data);
    ASSERT_TRUE(retried);
    EXPECT_EQ(retried->status, 204);
    EXPECT_TRUE(claims_are(gw, 0));
}

TEST(GatewayClaims, TheGaugeReturnsToZeroWhenTheDrainDeadlineAbortsTheChunk) {
    GatewayOptions options = fake_store(true);
    // Only the deadline may end this chunk.
    options.limits.body_idle_timeout = std::chrono::hours(1);
    options.limits.min_body_bytes_per_second = 0;
    GatewayUnderTest gw(options);
    HttpClient c(gw.endpoint());
    const auto up = create_upload(c, kMiB);
    ASSERT_TRUE(up);
    ASSERT_TRUE(c.send_raw(patch_head(*up, 0, 1000) + "abc"));
    ASSERT_TRUE(ulw::test::eventually([&] { return claims_are(gw, 1); }));
    gw.drain();
    gw.advance(options.limits.drain_deadline);
    EXPECT_TRUE(c.closed_by_peer());
    EXPECT_TRUE(ulw::test::eventually([&] { return claims_are(gw, 0); }));
}

TEST(GatewayClaims, TheGaugeReturnsToZeroWhenTheBackstopEndsTheChunk) {
    GatewayOptions options = fake_store(true);
    // Only the backstop may end this chunk: a byte every three hours keeps the body alive.
    options.limits.body_idle_timeout = std::chrono::hours(4);
    options.limits.min_body_bytes_per_second = 0;
    GatewayUnderTest gw(options);
    HttpClient c(gw.endpoint());
    const auto up = create_upload(c, kMiB);
    ASSERT_TRUE(up);
    ASSERT_TRUE(c.send_raw(patch_head(*up, 0, 1000) + "abc"));
    ASSERT_TRUE(ulw::test::eventually([&] { return claims_are(gw, 1); }));
    const std::uint64_t before = gw.counters().bytes_ingested;
    gw.advance(std::chrono::hours(3));
    ASSERT_TRUE(c.send_raw("d"));
    ASSERT_TRUE(ulw::test::eventually([&] { return gw.counters().bytes_ingested == before + 1; }));
    EXPECT_TRUE(claims_are(gw, 1));
    gw.advance(std::chrono::hours(3));
    EXPECT_TRUE(c.closed_by_peer());
    EXPECT_TRUE(ulw::test::eventually([&] { return claims_are(gw, 0); }));
    EXPECT_EQ(gw.counters().timeouts_backstop, 1U);
}

// A gauge and regression test, not a reproduction: a request whose storage job is still
// running is ended by the backstop, and the job completes while a request on another
// connection holds a claim. Claims are per connection, so the old code passed this too; the
// stale release itself is decided by UploadClaim.AStaleCompletionCannotReleaseALaterRequestsClaim
// in upload_claim_test.cpp.
TEST(GatewayClaims, AStaleJobCompletionKeepsTheGaugeOnTheClaimStillHeld) {
    GatewayUnderTest gw(fake_store(true));
    gw.put_video(core::VideoRecord{.id = *core::VideoId::parse(kVideo),
                                   .owner = *core::UserId::parse("alice"),
                                   .title = "trip",
                                   .state = core::VideoState::Ready,
                                   .version = 3,
                                   .error_reason = std::nullopt,
                                   .duration = core::Millis{5'000}});
    gw.put_object("videos/" + std::string(kVideo) + "/hls/master.m3u8", kMaster);
    gw.hold_fetches(true);
    HttpClient viewer(gw.endpoint());
    ASSERT_TRUE(viewer.send_raw("GET /api/v1/videos/" + std::string(kVideo) +
                                "/master.m3u8 HTTP/1.1\r\nHost: t\r\n"
                                "Authorization: Bearer user.alice\r\n\r\n"));
    ASSERT_TRUE(ulw::test::eventually([&] { return gw.held_fetches() == 1; }));
    gw.advance(gateway::Limits{}.request_backstop);
    EXPECT_TRUE(viewer.closed_by_peer());

    HttpClient uploader(gw.endpoint());
    const auto up = create_upload(uploader, kMiB);
    ASSERT_TRUE(up);
    ASSERT_TRUE(uploader.send_raw(patch_head(*up, 0, kMiB)));
    ASSERT_TRUE(ulw::test::eventually([&] { return claims_are(gw, 1); }));

    gw.hold_fetches(false);
    ASSERT_TRUE(ulw::test::eventually([&] { return gw.connections() == 1; }));
    EXPECT_TRUE(claims_are(gw, 1));

    const auto data = ulw::test::pattern(kMiB);
    ASSERT_TRUE(uploader.send_raw(data));
    const auto r = uploader.read_response();
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 204);
    EXPECT_TRUE(claims_are(gw, 0));
}

} // namespace
