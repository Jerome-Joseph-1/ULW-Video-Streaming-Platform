#include "core/util/json.hpp"
#include "core/util/parse.hpp"

#include "gateway_harness.hpp"
#include "support/eventually.hpp"
#include "support/http_client.hpp"
#include "support/reactor_harness.hpp"

#include <charconv>
#include <gtest/gtest.h>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ulw::test::Backend;
using ulw::test::GatewayOptions;
using ulw::test::GatewayUnderTest;
using ulw::test::HttpClient;
using ulw::test::kMiB;

constexpr std::string_view kAlice = "user.alice";
constexpr std::string_view kVideo = "01890a5d-ac96-774b-bcce-b302099a8057";

// One family of a scrape: what its HELP and TYPE lines said, and its samples.
struct Family {
    std::string help;
    std::string type;
    std::vector<std::pair<std::string, double>> samples;
};

std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> out;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        out.push_back(text.substr(0, nl));
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
    }
    return out;
}

// The family a sample line belongs to: its name less a histogram suffix.
std::string family_of(std::string_view series, const std::map<std::string, Family>& families) {
    const std::string_view name = series.substr(0, series.find('{'));
    for (const std::string_view suffix : {"_bucket", "_sum", "_count"}) {
        if (name.ends_with(suffix)) {
            const std::string base(name.substr(0, name.size() - suffix.size()));
            if (families.contains(base) && families.at(base).type == "histogram") {
                return base;
            }
        }
    }
    return std::string(name);
}

// Parses the exposition strictly: every sample must follow its family's HELP and TYPE, and
// every value must be a number.
std::map<std::string, Family> parse_exposition(std::string_view text) {
    std::map<std::string, Family> families;
    std::string current;
    for (const std::string_view line : split_lines(text)) {
        if (line.starts_with("# HELP ")) {
            const std::string_view rest = line.substr(7);
            current = std::string(rest.substr(0, rest.find(' ')));
            EXPECT_FALSE(families.contains(current)) << "family declared twice: " << current;
            families[current].help = std::string(rest.substr(rest.find(' ') + 1));
            continue;
        }
        if (line.starts_with("# TYPE ")) {
            const std::string_view rest = line.substr(7);
            EXPECT_EQ(rest.substr(0, rest.find(' ')), current) << "TYPE without its HELP";
            families[current].type = std::string(rest.substr(rest.find(' ') + 1));
            continue;
        }
        const std::size_t space = line.rfind(' ');
        EXPECT_NE(space, std::string_view::npos) << line;
        const std::string_view series = line.substr(0, space);
        const std::string_view value = line.substr(space + 1);
        double v = 0;
        const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), v);
        EXPECT_TRUE(ec == std::errc{} && ptr == value.data() + value.size()) << line;
        const std::string family = family_of(series, families);
        EXPECT_EQ(family, current) << "sample outside its family: " << line;
        families[family].samples.emplace_back(std::string(series), v);
    }
    return families;
}

std::string scrape(const GatewayUnderTest& gw) {
    HttpClient c(gw.endpoint());
    const auto r = c.request("GET", "/metrics", "");
    EXPECT_TRUE(r && r->status == 200);
    EXPECT_EQ(r->header("content-type"), "text/plain; version=0.0.4");
    return r ? r->body : std::string{};
}

double value_of(const std::map<std::string, Family>& families, const std::string& series) {
    const std::string family = family_of(series, families);
    for (const auto& [name, v] : families.at(family).samples) {
        if (name == series) {
            return v;
        }
    }
    ADD_FAILURE() << "no sample " << series;
    return -1;
}

TEST(GatewayMetrics, EveryOperationsFamilyIsScrapedWithItsHelpAndType) {
    const GatewayUnderTest gw(GatewayOptions{});
    const auto families = parse_exposition(scrape(gw));
    const std::map<std::string, std::string> expected{
        {"build_info", "gauge"},
        {"ready", "gauge"},
        {"dependency_up", "gauge"},
        {"requests_total", "counter"},
        {"responses_total", "counter"},
        {"connections_accepted_total", "counter"},
        {"connections_rejected_total", "counter"},
        {"connections_current", "gauge"},
        {"uploads_in_flight", "gauge"},
        {"admission_rejections_total", "counter"},
        {"rate_limited_total", "counter"},
        {"rate_limit_entries", "gauge"},
        {"rate_limit_evictions_total", "counter"},
        {"bytes_ingested_total", "counter"},
        {"part_upload_duration_seconds", "histogram"},
        {"backend_write_stall_seconds", "histogram"},
        {"buffer_bytes_in_use", "gauge"},
        {"timeouts_total", "counter"},
        {"tls_handshakes_in_flight", "gauge"},
        {"tls_handshake_failures_total", "counter"},
        {"certificate_reloads_total", "counter"},
        {"certificate_reload_failures_total", "counter"},
        {"playlist_requests_total", "counter"},
        {"playlists_rejected_total", "counter"},
        {"presign_failures_total", "counter"},
        {"live_playlist_cache_hits_total", "counter"},
        {"live_playlist_cache_misses_total", "counter"},
        {"live_playlist_fetches_total", "counter"},
        {"live_playlist_single_flight_joins_total", "counter"},
        {"live_playlist_cache_evictions_total", "counter"},
        {"live_playlist_cache_entries", "gauge"},
        {"live_playlist_cache_bytes", "gauge"},
        {"view_events_recorded_total", "counter"},
        {"view_events_dropped_total", "counter"},
        {"view_batches_failed_total", "counter"},
        {"jobs_oldest_queued_seconds", "gauge"},
        {"store_paging_errors_total", "counter"},
        {"log_messages_dropped_total", "counter"},
        {"open_fds", "gauge"},
        {"resident_memory_bytes", "gauge"},
    };
    for (const auto& [name, type] : expected) {
        ASSERT_TRUE(families.contains(name)) << name;
        EXPECT_EQ(families.at(name).type, type) << name;
        EXPECT_FALSE(families.at(name).help.empty()) << name;
        EXPECT_FALSE(families.at(name).samples.empty()) << name;
    }
    std::set<std::string> extra;
    for (const auto& [name, family] : families) {
        if (!expected.contains(name)) {
            extra.insert(name);
        }
    }
    EXPECT_TRUE(extra.empty()) << "families nobody documented: " << *extra.begin();
}

TEST(GatewayMetrics, CountersFollowTheTrafficTheyCount) {
    const GatewayUnderTest gw(GatewayOptions{});
    HttpClient c(gw.endpoint());
    ASSERT_EQ(c.request("GET", "/api/v1/healthz", "")->status, 200);
    ASSERT_EQ(c.request("GET", "/nowhere", "")->status, 404);
    const auto families = parse_exposition(scrape(gw));
    // The scrape itself is the third request and the second connection, and is counted
    // before it is answered.
    EXPECT_EQ(value_of(families, "requests_total"), 3);
    EXPECT_EQ(value_of(families, R"(responses_total{class="2xx"})"), 1);
    EXPECT_EQ(value_of(families, R"(responses_total{class="4xx"})"), 1);
    EXPECT_EQ(value_of(families, "connections_accepted_total"), 2);
    EXPECT_EQ(value_of(families, "ready"), 1);
    EXPECT_EQ(value_of(families, R"(dependency_up{dependency="database"})"), 1);
    const auto& build = families.at("build_info").samples.at(0).first;
    EXPECT_NE(build.find(R"(git_sha=")"), std::string::npos) << build;
}

TEST(GatewayMetrics, TheQueueAgeIsUnknownRatherThanStaleOrZeroWithoutADatabase) {
    // The harness's probe has not asked a database, so there is no age to report.
    const GatewayUnderTest gw(GatewayOptions{});
    const std::string m = scrape(gw);
    EXPECT_NE(m.find("\njobs_oldest_queued_seconds NaN\n"), std::string::npos) << m;
}

TEST(GatewayMetrics, ChunkUploadsAreTimedIntoTheHistogram) {
    const GatewayUnderTest gw(GatewayOptions{.backend = Backend::Fake, .chunk = kMiB});
    HttpClient c(gw.endpoint());
    const std::string body = R"({"filename":"a.mp4","size_bytes":)" + std::to_string(2 * kMiB) +
                             R"(,"content_type":"video/mp4"})";
    const auto created =
        c.request("POST", "/api/v1/uploads", kAlice, std::as_bytes(std::span(body)));
    ASSERT_TRUE(created && created->status == 201);
    const auto doc = core::json::parse(created->body);
    const std::string id(*doc->find("upload_id")->as_string());
    const auto data = ulw::test::pattern(kMiB);
    for (const std::uint64_t offset : {std::uint64_t{0}, kMiB}) {
        const auto r = c.request("PATCH", "/api/v1/uploads/" + id, kAlice, data,
                                 {{"Upload-Offset", std::to_string(offset)}});
        ASSERT_TRUE(r && r->status == 204);
    }
    const auto families = parse_exposition(scrape(gw));
    EXPECT_EQ(value_of(families, "part_upload_duration_seconds_count"), 2);
    EXPECT_EQ(value_of(families, R"(part_upload_duration_seconds_bucket{le="+Inf"})"), 2);
    EXPECT_EQ(value_of(families, "bytes_ingested_total"), 2.0 * kMiB);
    // Buckets are cumulative: never fewer below a larger bound.
    double last = 0;
    for (const auto& [series, v] : families.at("part_upload_duration_seconds").samples) {
        if (series.find("_bucket") != std::string::npos) {
            EXPECT_GE(v, last) << series;
            last = v;
        }
    }
}

class GatewayReadiness : public ::testing::Test {
protected:
    static std::pair<int, std::string> readyz(const GatewayUnderTest& gw,
                                              std::string_view path = "/api/v1/readyz") {
        HttpClient c(gw.endpoint());
        const auto r = c.request("GET", path, "");
        return r ? std::pair{r->status, r->body} : std::pair{0, std::string{}};
    }
};

TEST_F(GatewayReadiness, NotReadyUntilTheFirstProbeHasAnswered) {
    GatewayUnderTest gw(GatewayOptions{.database_up = std::nullopt, .store_up = std::nullopt});
    EXPECT_EQ(readyz(gw), std::pair(503, std::string("starting\n")));
    HttpClient c(gw.endpoint());
    EXPECT_EQ(c.request("GET", "/api/v1/healthz", "")->status, 200);
    gw.set_health(true, true);
    EXPECT_EQ(readyz(gw), std::pair(200, std::string("ready\n")));
}

TEST_F(GatewayReadiness, AnUnreachableDependencyIsNamed) {
    GatewayUnderTest gw(GatewayOptions{});
    gw.set_health(false, true);
    EXPECT_EQ(readyz(gw), std::pair(503, std::string("database unreachable\n")));
    gw.set_health(true, false);
    EXPECT_EQ(readyz(gw), std::pair(503, std::string("object store unreachable\n")));
    gw.set_health(true, true);
    EXPECT_EQ(readyz(gw), std::pair(200, std::string("ready\n")));
}

TEST_F(GatewayReadiness, AProbeThatStopsAnsweringIsNotTakenAsHealthForever) {
    GatewayUnderTest gw(GatewayOptions{.manual_clock = true});
    gw.set_health(std::nullopt, std::nullopt);
    gw.advance(gateway::kProbeStale);
    EXPECT_EQ(readyz(gw).first, 200);
    gw.advance(core::Millis{1});
    EXPECT_EQ(readyz(gw), std::pair(503, std::string("health probe stuck\n")));
}

TEST_F(GatewayReadiness, DrainingWinsOverAHealthyProbe) {
    GatewayUnderTest gw(GatewayOptions{});
    HttpClient c(gw.endpoint());
    // Half a request, so the connection is busy and outlives the drain. Accepted is not busy:
    // until the gateway has read those bytes the connection is idle, and a drain closes it.
    ASSERT_TRUE(c.send_raw("GET /readyz HTTP/1.1\r\nHost: t\r\n"));
    ASSERT_TRUE(ulw::test::eventually([&] { return gw.busy_connections() == 1; }));
    gw.drain();
    ASSERT_TRUE(c.send_raw("\r\n"));
    const auto r = c.read_response();
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 503);
    EXPECT_EQ(r->body, "draining\n");
}

TEST_F(GatewayReadiness, TheUnprefixedProbePathsAnswerLikeTheApiOnes) {
    GatewayUnderTest gw(GatewayOptions{});
    EXPECT_EQ(readyz(gw, "/readyz"), std::pair(200, std::string("ready\n")));
    HttpClient c(gw.endpoint());
    EXPECT_EQ(c.request("GET", "/healthz", "")->status, 200);
    gw.set_health(false, true);
    EXPECT_EQ(readyz(gw, "/readyz").first, 503);
}

TEST(GatewayLog, EveryResponseIsOneLineCarryingItsRequestId) {
    const GatewayUnderTest gw(GatewayOptions{});
    HttpClient c(gw.endpoint());
    const auto r = c.request("GET", "/api/v1/videos/" + std::string(kVideo), kAlice);
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 404);
    const auto id = r->header("x-request-id");
    ASSERT_TRUE(id);
    ASSERT_TRUE(ulw::test::eventually([&] { return !gw.log().events("request").empty(); }));
    const auto lines = gw.log().events("request");
    ASSERT_EQ(lines.size(), 1U);
    const auto doc = core::json::parse(lines[0]);
    ASSERT_TRUE(doc) << lines[0];
    EXPECT_EQ(doc->find("request_id")->as_string(), *id);
    EXPECT_EQ(doc->find("method")->as_string(), "GET");
    EXPECT_EQ(doc->find("route")->as_string(), "get_video");
    EXPECT_EQ(doc->find("status")->as_u64(), 404U);
    EXPECT_EQ(doc->find("level")->as_string(), "info");
}

core::VideoRecord ready_video() {
    return core::VideoRecord{.id = *core::VideoId::parse(kVideo),
                             .owner = *core::UserId::parse("alice"),
                             .title = "t",
                             .state = core::VideoState::Ready,
                             .version = 3,
                             .error_reason = std::nullopt,
                             .duration = core::Millis{5'000}};
}

TEST(GatewayLog, AServerErrorIsAWarning) {
    GatewayUnderTest gw(GatewayOptions{
        .backend = Backend::Fake, .plan = {.fail_fetch = core::ports::StorageError::Transient}});
    gw.put_video(ready_video());
    HttpClient c(gw.endpoint());
    const auto r =
        c.request("GET", "/api/v1/videos/" + std::string(kVideo) + "/master.m3u8", kAlice);
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 503);
    ASSERT_TRUE(ulw::test::eventually([&] { return !gw.log().events("request").empty(); }));
    const auto doc = core::json::parse(gw.log().events("request").at(0));
    ASSERT_TRUE(doc);
    EXPECT_EQ(doc->find("level")->as_string(), "warn");
    EXPECT_EQ(doc->find("route")->as_string(), "master_playlist");
}

TEST(GatewayLog, ProbesAndScrapesAreDebugNoise) {
    const GatewayUnderTest gw(GatewayOptions{});
    HttpClient c(gw.endpoint());
    ASSERT_EQ(c.request("GET", "/readyz", "")->status, 200);
    ASSERT_TRUE(ulw::test::eventually([&] { return !gw.log().events("request").empty(); }));
    const auto doc = core::json::parse(gw.log().events("request").at(0));
    ASSERT_TRUE(doc);
    EXPECT_EQ(doc->find("level")->as_string(), "debug");
}

// Everything that could carry a credential or a user's content goes through the gateway, at
// the most verbose level, and none of it may appear in any line.
TEST(GatewayLog, NoTokenHeaderSignedUrlOrBodyEverReachesTheLog) {
    GatewayUnderTest gw(GatewayOptions{.backend = Backend::Fake, .chunk = kMiB});
    constexpr std::string_view kTitle = "title-marker-7b1c.mp4";
    constexpr std::string_view kBodyMarker = "BODY-MARKER-3f9a";
    HttpClient c(gw.endpoint());

    const std::string create = R"({"filename":")" + std::string(kTitle) + R"(","size_bytes":)" +
                               std::to_string(kMiB) + R"(,"content_type":"video/mp4"})";
    const auto created =
        c.request("POST", "/api/v1/uploads", kAlice, std::as_bytes(std::span(create)));
    ASSERT_TRUE(created && created->status == 201);
    const auto doc = core::json::parse(created->body);
    const std::string id(*doc->find("upload_id")->as_string());
    std::string chunk(kMiB, 'x');
    chunk.replace(0, kBodyMarker.size(), kBodyMarker);
    ASSERT_EQ(c.request("PATCH", "/api/v1/uploads/" + id, kAlice, std::as_bytes(std::span(chunk)),
                        {{"Upload-Offset", "0"}})
                  ->status,
              204);
    // The cookie form of the token, a token that fails, and a request that fails to parse.
    HttpClient cookie(gw.endpoint());
    EXPECT_EQ(cookie
                  .request("GET", "/api/v1/videos/" + std::string(kVideo), "", {},
                           {{"Cookie", "auth_token=user.carol"}})
                  ->status,
              404);
    HttpClient bad(gw.endpoint());
    EXPECT_EQ(bad.request("GET", "/api/v1/videos/" + std::string(kVideo), "forged.!sig")->status,
              401);
    HttpClient broken(gw.endpoint());
    ASSERT_TRUE(broken.send_raw("GET /x HTTP/1.1\r\nAuthorization: Bearer user.dave\r\n"
                                "Host: a\r\nHost: b\r\n\r\n"));
    EXPECT_EQ(broken.read_response()->status, 400);

    // A playlist, whose every URI is signed for the viewer.
    gw.put_video(ready_video());
    const std::string prefix = "videos/" + std::string(kVideo) + "/hls/";
    gw.put_object(prefix + "master.m3u8",
                  "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1,RESOLUTION=2x2\n720p/index.m3u8\n");
    gw.put_object(prefix + "720p/index.m3u8",
                  "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXTINF:4.0,\nseg_00000.m4s\n"
                  "#EXT-X-ENDLIST\n");
    const auto media =
        c.request("GET", "/api/v1/videos/" + std::string(kVideo) + "/720p/index.m3u8", kAlice);
    ASSERT_TRUE(media && media->status == 200);
    ASSERT_NE(media->body.find("fake://"), std::string::npos);

    ASSERT_TRUE(ulw::test::eventually([&] { return gw.log().events("request").size() >= 6; }));
    const std::string all = gw.log().all();
    for (const std::string_view secret :
         {std::string_view("user.alice"), std::string_view("user.carol"),
          std::string_view("user.dave"), std::string_view("forged.!sig"),
          std::string_view("Bearer"), std::string_view("uthorization"),
          std::string_view("auth_token"), std::string_view("fake://"), kBodyMarker, kTitle}) {
        EXPECT_EQ(all.find(secret), std::string::npos) << secret << " in:\n" << all;
    }
}

} // namespace
