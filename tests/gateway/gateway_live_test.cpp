#include "core/util/parse.hpp"

#include "gateway_harness.hpp"
#include "support/http_client.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ulw::test::Backend;
using ulw::test::GatewayOptions;
using ulw::test::GatewayUnderTest;
using ulw::test::HttpClient;

constexpr std::string_view kAlice = "user.alice";
constexpr std::string_view kBob = "user.bob";

// As live_packager writes it, two segments into a 2 s stream.
constexpr std::string_view kLive = "#EXTM3U\n"
                                   "#EXT-X-VERSION:7\n"
                                   "#EXT-X-TARGETDURATION:2\n"
                                   "#EXT-X-MEDIA-SEQUENCE:4\n"
                                   "#EXT-X-INDEPENDENT-SEGMENTS\n"
                                   "#EXT-X-MAP:URI=\"init_1.mp4\"\n"
                                   "#EXT-X-PROGRAM-DATE-TIME:2026-09-29T12:00:08.000Z\n"
                                   "#EXTINF:2.000,\n"
                                   "seg_1_4.m4s\n"
                                   "#EXT-X-PROGRAM-DATE-TIME:2026-09-29T12:00:10.000Z\n"
                                   "#EXTINF:2.000,\n"
                                   "seg_1_5.m4s\n";

std::string path(std::string_view stream) {
    return "/api/v1/live/" + std::string(stream) + "/index.m3u8";
}

std::string key(std::string_view stream, std::string_view rest = "index.m3u8") {
    return "live/" + std::string(stream) + "/" + std::string(rest);
}

// The value of an unlabelled sample in the exposition text.
std::uint64_t metric(const std::string& text, std::string_view name) {
    const std::string needle = "\n" + std::string(name) + " ";
    const std::size_t at = text.find(needle);
    if (at == std::string::npos) {
        ADD_FAILURE() << name << " missing";
        return 0;
    }
    const std::size_t from = at + needle.size();
    const auto value = core::parse_integer<std::uint64_t>(
        std::string_view(text).substr(from, text.find('\n', from) - from));
    EXPECT_TRUE(value) << name;
    return value.value_or(0);
}

class GatewayLive : public ::testing::TestWithParam<gateway::Transport> {
protected:
    [[nodiscard]] static GatewayOptions options() {
        GatewayOptions o;
        o.backend = Backend::Fake;
        o.transport = GetParam();
        // Freshness is judged on the gateway's clock; only the test moves it.
        o.manual_clock = true;
        return o;
    }
};

TEST_P(GatewayLive, ThePlaylistIsServedWithEveryUriSignedAndNoViewerCaching) {
    GatewayUnderTest gw(options());
    gw.put_object(key("show"), kLive);
    HttpClient c(gw.endpoint());
    const auto r = c.request("GET", path("show"), kAlice);
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    EXPECT_EQ(r->header("content-type"), "application/vnd.apple.mpegurl");
    EXPECT_EQ(r->header("cache-control"), "private, no-cache");
    // The fake store grants fake://<key>.
    EXPECT_EQ(r->body, "#EXTM3U\n"
                       "#EXT-X-VERSION:7\n"
                       "#EXT-X-TARGETDURATION:2\n"
                       "#EXT-X-MEDIA-SEQUENCE:4\n"
                       "#EXT-X-INDEPENDENT-SEGMENTS\n"
                       "#EXT-X-MAP:URI=\"fake://" +
                           key("show", "init_1.mp4") +
                           "\"\n"
                           "#EXT-X-PROGRAM-DATE-TIME:2026-09-29T12:00:08.000Z\n"
                           "#EXTINF:2.000,\n"
                           "fake://" +
                           key("show", "seg_1_4.m4s") +
                           "\n"
                           "#EXT-X-PROGRAM-DATE-TIME:2026-09-29T12:00:10.000Z\n"
                           "#EXTINF:2.000,\n"
                           "fake://" +
                           key("show", "seg_1_5.m4s") + "\n");
}

TEST_P(GatewayLive, AnySignedInViewerMayWatchButNobodyAnonymous) {
    GatewayUnderTest gw(options());
    gw.put_object(key("show"), kLive);
    HttpClient a(gw.endpoint());
    EXPECT_EQ(a.request("GET", path("show"), kAlice)->status, 200);
    HttpClient b(gw.endpoint());
    EXPECT_EQ(b.request("GET", path("show"), kBob)->status, 200);
    HttpClient c(gw.endpoint());
    EXPECT_EQ(c.request("GET", path("show"), "")->status, 401);
    HttpClient d(gw.endpoint());
    EXPECT_EQ(d.request("GET", path("show"), "forged")->status, 401);
}

TEST_P(GatewayLive, AStreamNotStartedAndAnIdNoPackagerTakesAreNotFound) {
    GatewayUnderTest gw(options());
    HttpClient c(gw.endpoint());
    EXPECT_EQ(c.request("GET", path("later"), kAlice)->status, 404);
    EXPECT_EQ(c.request("GET", path("a.b"), kAlice)->status, 404);
    EXPECT_EQ(c.request("GET", path(std::string(65, 'x')), kAlice)->status, 404);
    // Neither bad id went near the store.
    EXPECT_EQ(metric(gw.metrics(), "live_playlist_fetches_total"), 1U);
}

TEST_P(GatewayLive, ManyViewersCostOneStoreReadPerFreshnessInterval) {
    GatewayUnderTest gw(options());
    gw.put_object(key("show"), kLive);
    constexpr int kViewers = 20;
    std::vector<std::unique_ptr<HttpClient>> viewers;
    for (int i = 0; i < kViewers; ++i) {
        viewers.push_back(std::make_unique<HttpClient>(gw.endpoint()));
        ASSERT_TRUE(viewers.back()->send_request("GET", path("show"), kAlice, {}));
    }
    std::string first;
    for (auto& v : viewers) {
        const auto r = v->read_response();
        ASSERT_TRUE(r);
        ASSERT_EQ(r->status, 200);
        if (first.empty()) {
            first = r->body;
        }
        EXPECT_EQ(r->body, first);
    }
    std::string m = gw.metrics();
    EXPECT_EQ(metric(m, "live_playlist_fetches_total"), 1U);
    EXPECT_EQ(metric(m, "live_playlist_cache_hits_total") +
                  metric(m, "live_playlist_single_flight_joins_total"),
              std::uint64_t{kViewers - 1});

    // T = 2 s: the copy is fresh for 1 s, then the next viewer reads the store again.
    gw.advance(core::Millis{999});
    EXPECT_EQ(viewers.front()->request("GET", path("show"), kBob)->status, 200);
    EXPECT_EQ(metric(gw.metrics(), "live_playlist_fetches_total"), 1U);
    gw.advance(core::Millis{1});
    EXPECT_EQ(viewers.front()->request("GET", path("show"), kBob)->status, 200);
    m = gw.metrics();
    EXPECT_EQ(metric(m, "live_playlist_fetches_total"), 2U);
    EXPECT_EQ(metric(m, "live_playlist_cache_entries"), 1U);
}

TEST_P(GatewayLive, AnEndedStreamKeepsItsEndlistAndMayBeCachedLikeVod) {
    GatewayUnderTest gw(options());
    gw.put_object(key("show"), std::string(kLive) + "#EXT-X-ENDLIST\n");
    HttpClient c(gw.endpoint());
    const auto r = c.request("GET", path("show"), kAlice);
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    EXPECT_EQ(r->header("cache-control"), "private, max-age=60");
    EXPECT_TRUE(r->body.ends_with("\n#EXT-X-ENDLIST\n")) << r->body;
}

TEST_P(GatewayLive, AStoreOutageIsRetryableAndNotRemembered) {
    GatewayUnderTest gw(options());
    gw.put_object(key("show"), kLive);
    gw.set_plan({.fail_fetch = core::ports::StorageError::Transient});
    HttpClient c(gw.endpoint());
    EXPECT_EQ(c.request("GET", path("show"), kAlice)->status, 503);
    gw.set_plan({});
    EXPECT_EQ(c.request("GET", path("show"), kAlice)->status, 200);
}

TEST_P(GatewayLive, APlaylistThatBreaksTheRewritingRulesIsCountedAndRefused) {
    GatewayUnderTest gw(options());
    gw.put_object(key("show"),
                  "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXTINF:2.000,\n../other/seg.m4s\n");
    HttpClient c(gw.endpoint());
    EXPECT_EQ(c.request("GET", path("show"), kAlice)->status, 500);
    EXPECT_EQ(gw.counters().playlists_rejected, 1U);
    EXPECT_EQ(gw.counters().playlists_live, 1U);
}

INSTANTIATE_TEST_SUITE_P(Transports, GatewayLive,
                         ::testing::Values(gateway::Transport::Plain, gateway::Transport::Tls),
                         [](const ::testing::TestParamInfo<gateway::Transport>& p) {
                             return p.param == gateway::Transport::Tls ? "Tls" : "Plain";
                         });

} // namespace
