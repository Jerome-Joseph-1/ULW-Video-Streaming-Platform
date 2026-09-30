#include "gateway_harness.hpp"
#include "playback.hpp"
#include "support/eventually.hpp"
#include "support/http_client.hpp"

#include <gtest/gtest.h>
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
constexpr std::string_view kVideo = "01890a5d-ac96-774b-bcce-b302099a8057";

constexpr std::string_view kMaster = "#EXTM3U\n"
                                     "#EXT-X-VERSION:7\n"
                                     "#EXT-X-STREAM-INF:BANDWIDTH=1020800,RESOLUTION=1280x720,"
                                     "CODECS=\"avc1.4d401f,mp4a.40.2\"\n"
                                     "720p/index.m3u8\n"
                                     "\n"
                                     "#EXT-X-STREAM-INF:BANDWIDTH=580800,RESOLUTION=640x360,"
                                     "CODECS=\"avc1.4d401e,mp4a.40.2\"\n"
                                     "360p/index.m3u8\n";

constexpr std::string_view kMedia = "#EXTM3U\n"
                                    "#EXT-X-VERSION:7\n"
                                    "#EXT-X-TARGETDURATION:4\n"
                                    "#EXT-X-PLAYLIST-TYPE:VOD\n"
                                    "#EXT-X-MAP:URI=\"init_0.mp4\"\n"
                                    "#EXTINF:4.000000,\n"
                                    "seg_00000.m4s\n"
                                    "#EXTINF:1.000000,\n"
                                    "seg_00001.m4s\n"
                                    "#EXT-X-ENDLIST\n";

std::string path(std::string_view rest, std::string_view video = kVideo) {
    return "/api/v1/videos/" + std::string(video) + "/" + std::string(rest);
}

std::string key(std::string_view rest, std::string_view video = kVideo) {
    return "videos/" + std::string(video) + "/hls/" + std::string(rest);
}

core::VideoRecord video(core::VideoState state, std::string_view owner = "alice") {
    const bool ready = state == core::VideoState::Ready;
    return core::VideoRecord{.id = *core::VideoId::parse(kVideo),
                             .owner = *core::UserId::parse(owner),
                             .title = "trip",
                             .state = state,
                             .version = 3,
                             .error_reason = state == core::VideoState::Failed
                                                 ? std::optional<std::string>("bad")
                                                 : std::nullopt,
                             .duration = ready ? std::optional(core::Millis{5'000}) : std::nullopt};
}

// Every behaviour below must hold whichever transport carries it; the harness takes its
// reactor from ULW_REACTOR, and CI runs the suite under both.
class GatewayPlayback : public ::testing::TestWithParam<gateway::Transport> {
protected:
    [[nodiscard]] static GatewayOptions options(Backend backend = Backend::Fake) {
        GatewayOptions o;
        o.backend = backend;
        o.transport = GetParam();
        return o;
    }

    // A ready video of Alice's with both playlists of one rung published.
    static void publish(GatewayUnderTest& gw, core::VideoState state = core::VideoState::Ready) {
        gw.put_video(video(state));
        gw.put_object(key("master.m3u8"), kMaster);
        gw.put_object(key("720p/index.m3u8"), kMedia);
    }
};

TEST_P(GatewayPlayback, AFailedVideoTellsItsOwnerWhy) {
    GatewayUnderTest gw(options());
    core::VideoRecord failed = video(core::VideoState::Failed);
    failed.error_reason = R"(the file could not be decoded as "video")";
    gw.put_video(failed);
    HttpClient c(gw.endpoint());
    const std::string resource = "/api/v1/videos/" + std::string(kVideo);
    const auto r = c.request("GET", resource, kAlice);
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    EXPECT_EQ(r->body, R"({"id":")" + std::string(kVideo) +
                           R"(","title":"trip","state":"failed","version":3,"duration_ms":null,)"
                           R"("error_reason":"the file could not be decoded as \"video\""})");

    // Any other state has no reason to give, and the field is left out.
    gw.put_video(video(core::VideoState::Ready));
    const auto ready = c.request("GET", resource, kAlice);
    ASSERT_TRUE(ready);
    ASSERT_EQ(ready->status, 200) << ready->body;
    EXPECT_EQ(ready->body,
              R"({"id":")" + std::string(kVideo) +
                  R"(","title":"trip","state":"ready","version":3,"duration_ms":5000})");
}

TEST_P(GatewayPlayback, MasterRoutesEachRenditionBackThroughTheGateway) {
    GatewayUnderTest gw(options());
    publish(gw);
    HttpClient c(gw.endpoint());
    const auto r = c.request("GET", path("master.m3u8"), kAlice);
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    EXPECT_EQ(r->header("content-type"), "application/vnd.apple.mpegurl");
    EXPECT_EQ(r->header("cache-control"), "private, max-age=60");
    EXPECT_EQ(r->body, "#EXTM3U\n"
                       "#EXT-X-VERSION:7\n"
                       "#EXT-X-STREAM-INF:BANDWIDTH=1020800,RESOLUTION=1280x720,"
                       "CODECS=\"avc1.4d401f,mp4a.40.2\"\n" +
                           path("720p/index.m3u8") +
                           "\n"
                           "#EXT-X-STREAM-INF:BANDWIDTH=580800,RESOLUTION=640x360,"
                           "CODECS=\"avc1.4d401e,mp4a.40.2\"\n" +
                           path("360p/index.m3u8") + "\n");
}

TEST_P(GatewayPlayback, MediaPlaylistCarriesSignedUrlsForTheInitAndEverySegment) {
    GatewayUnderTest gw(options());
    publish(gw);
    HttpClient c(gw.endpoint());
    const auto r = c.request("GET", path("720p/index.m3u8"), kAlice);
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    EXPECT_EQ(r->header("content-type"), "application/vnd.apple.mpegurl");
    EXPECT_EQ(r->header("cache-control"), "private, max-age=60");
    // The fake store grants fake://<key>.
    EXPECT_EQ(r->body, "#EXTM3U\n"
                       "#EXT-X-VERSION:7\n"
                       "#EXT-X-TARGETDURATION:4\n"
                       "#EXT-X-PLAYLIST-TYPE:VOD\n"
                       "#EXT-X-MAP:URI=\"fake://" +
                           key("720p/init_0.mp4") +
                           "\"\n"
                           "#EXTINF:4.000000,\n"
                           "fake://" +
                           key("720p/seg_00000.m4s") +
                           "\n"
                           "#EXTINF:1.000000,\n"
                           "fake://" +
                           key("720p/seg_00001.m4s") +
                           "\n"
                           "#EXT-X-ENDLIST\n");
}

TEST_P(GatewayPlayback, SegmentUrlsComeFromTheFileServerTheFilesystemBackendNames) {
    GatewayOptions o = options(Backend::Fs);
    o.limits.local_read_url = "http://127.0.0.1:8081/objects";
    GatewayUnderTest gw(o);
    publish(gw);
    HttpClient c(gw.endpoint());
    const auto r = c.request("GET", path("720p/index.m3u8"), kAlice);
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    EXPECT_NE(r->body.find("\nhttp://127.0.0.1:8081/objects/" + key("720p/seg_00001.m4s") + "\n"),
              std::string::npos)
        << r->body;
}

TEST_P(GatewayPlayback, AStoreThatCannotGrantUrlsIsAServerErrorAndCounted) {
    // The filesystem backend grants file paths, and no file server is configured.
    GatewayUnderTest gw(options(Backend::Fs));
    publish(gw);
    HttpClient c(gw.endpoint());
    EXPECT_EQ(c.request("GET", path("720p/index.m3u8"), kAlice)->status, 500);
    EXPECT_EQ(gw.counters().presign_failures, 1U);
    // The master holds no segment URLs, so it still plays up to that point.
    EXPECT_EQ(c.request("GET", path("master.m3u8"), kAlice)->status, 200);
    EXPECT_NE(gw.metrics().find("presign_failures_total 1\n"), std::string::npos);
}

TEST_P(GatewayPlayback, PlaylistsNeedAToken) {
    GatewayUnderTest gw(options());
    publish(gw);
    HttpClient c(gw.endpoint());
    EXPECT_EQ(c.request("GET", path("master.m3u8"), "")->status, 401);
    HttpClient d(gw.endpoint());
    EXPECT_EQ(d.request("GET", path("720p/index.m3u8"), "forged")->status, 401);
}

TEST_P(GatewayPlayback, AnotherUsersVideoIsIndistinguishableFromAMissingOne) {
    GatewayUnderTest gw(options());
    publish(gw);
    HttpClient c(gw.endpoint());
    const auto theirs = c.request("GET", path("master.m3u8"), kBob);
    const auto missing =
        c.request("GET", path("master.m3u8", "01890a5d-ac96-774b-bcce-b302099a8058"), kBob);
    ASSERT_TRUE(theirs && missing);
    EXPECT_EQ(theirs->status, 404);
    EXPECT_EQ(missing->status, 404);
    EXPECT_EQ(theirs->body, missing->body);
    EXPECT_EQ(c.request("GET", path("720p/index.m3u8"), kBob)->status, 404);
    EXPECT_EQ(c.request("GET", path("master.m3u8", "not-a-uuid"), kAlice)->status, 404);
}

TEST_P(GatewayPlayback, AVideoThatIsNotReadyIsAConflictForItsOwner) {
    for (const core::VideoState state : {core::VideoState::Init, core::VideoState::Uploading,
                                         core::VideoState::Processing, core::VideoState::Failed}) {
        GatewayUnderTest gw(options());
        publish(gw, state);
        HttpClient c(gw.endpoint());
        EXPECT_EQ(c.request("GET", path("master.m3u8"), kAlice)->status, 409);
        EXPECT_EQ(c.request("GET", path("720p/index.m3u8"), kAlice)->status, 409);
        // Nobody else learns even that much.
        EXPECT_EQ(c.request("GET", path("master.m3u8"), kBob)->status, 404);
    }
}

TEST_P(GatewayPlayback, OnlyRenditionsTheMasterListsAreServed) {
    GatewayUnderTest gw(options());
    publish(gw);
    // Published, but the master does not name it.
    gw.put_object(key("1080p/index.m3u8"), kMedia);
    HttpClient c(gw.endpoint());
    for (const std::string_view r :
         {"1080p", "..", "%2e%2e", "720P", "720p%2Findex.m3u8%23", "master.m3u8"}) {
        EXPECT_EQ(c.request("GET", path(std::string(r) + "/index.m3u8"), kAlice)->status, 404) << r;
    }
}

TEST_P(GatewayPlayback, ARenditionTheMasterRoutesFromATagIsServed) {
    GatewayUnderTest gw(options());
    publish(gw);
    gw.put_object(key("master.m3u8"), std::string(kMaster) +
                                          "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"en\","
                                          "URI=\"audio/index.m3u8\"\n");
    gw.put_object(key("audio/index.m3u8"), kMedia);
    HttpClient c(gw.endpoint());
    const auto master = c.request("GET", path("master.m3u8"), kAlice);
    ASSERT_EQ(master->status, 200);
    ASSERT_NE(master->body.find("URI=\"" + path("audio/index.m3u8") + "\""), std::string::npos);
    EXPECT_EQ(c.request("GET", path("audio/index.m3u8"), kAlice)->status, 200);
}

TEST_P(GatewayPlayback, AStoredPlaylistPointingOutsideItsVideoIsRefusedAndCounted) {
    GatewayUnderTest gw(options());
    publish(gw);
    gw.put_object(key("720p/index.m3u8"),
                  "#EXTM3U\n#EXTINF:4,\n../../01890a5d-ac96-774b-bcce-b302099a8058/raw\n");
    HttpClient c(gw.endpoint());
    EXPECT_EQ(c.request("GET", path("720p/index.m3u8"), kAlice)->status, 500);
    EXPECT_EQ(gw.counters().playlists_rejected, 1U);
    EXPECT_NE(gw.metrics().find("playlists_rejected_total 1\n"), std::string::npos);
}

TEST_P(GatewayPlayback, AStoredPlaylistWithATagThatCouldRedirectThePlayerIsRefused) {
    GatewayUnderTest gw(options());
    publish(gw);
    gw.put_object(key("master.m3u8"),
                  std::string(kMaster) +
                      "#EXT-X-CONTENT-STEERING:SERVER-URI=\"https://evil.example/steer\"\n");
    HttpClient c(gw.endpoint());
    EXPECT_EQ(c.request("GET", path("master.m3u8"), kAlice)->status, 500);
    EXPECT_EQ(gw.counters().playlists_rejected, 1U);
}

TEST_P(GatewayPlayback, AReadyVideoWithoutItsPlaylistsIsAServerError) {
    GatewayUnderTest gw(options());
    gw.put_video(video(core::VideoState::Ready));
    HttpClient c(gw.endpoint());
    EXPECT_EQ(c.request("GET", path("master.m3u8"), kAlice)->status, 500);
    gw.put_object(key("master.m3u8"), kMaster);
    EXPECT_EQ(c.request("GET", path("360p/index.m3u8"), kAlice)->status, 500);
}

TEST_P(GatewayPlayback, AnOversizedStoredPlaylistIsNotRead) {
    GatewayUnderTest gw(options());
    publish(gw);
    std::string huge(gateway::kMaxStoredPlaylist + 1, '#');
    huge.replace(0, 8, "#EXTM3U\n");
    gw.put_object(key("master.m3u8"), huge);
    HttpClient c(gw.endpoint());
    EXPECT_EQ(c.request("GET", path("master.m3u8"), kAlice)->status, 500);
}

TEST_P(GatewayPlayback, AnUnreachableStoreIsARetry) {
    GatewayUnderTest gw(options());
    publish(gw);
    gw.set_plan({.fail_fetch = core::ports::StorageError::Transient});
    HttpClient c(gw.endpoint());
    const auto r = c.request("GET", path("master.m3u8"), kAlice);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 503);
    EXPECT_EQ(r->header("retry-after"), "5");
}

TEST_P(GatewayPlayback, EachPlaylistRequestIsCountedByKind) {
    GatewayUnderTest gw(options());
    publish(gw);
    HttpClient c(gw.endpoint());
    ASSERT_EQ(c.request("GET", path("master.m3u8"), kAlice)->status, 200);
    ASSERT_EQ(c.request("GET", path("720p/index.m3u8"), kAlice)->status, 200);
    ASSERT_EQ(c.request("GET", path("720p/index.m3u8"), kBob)->status, 404);
    const std::string m = gw.metrics();
    EXPECT_NE(m.find("playlist_requests_total{kind=\"master\"} 1\n"), std::string::npos) << m;
    EXPECT_NE(m.find("playlist_requests_total{kind=\"media\"} 2\n"), std::string::npos) << m;
}

TEST_P(GatewayPlayback, AMasterFetchIsOneViewWrittenInTheNextBatch) {
    GatewayOptions o = options();
    o.manual_clock = true;
    GatewayUnderTest gw(o);
    publish(gw);
    HttpClient c(gw.endpoint());
    ASSERT_EQ(c.request("GET", path("master.m3u8"), kAlice)->status, 200);
    ASSERT_EQ(c.request("GET", path("720p/index.m3u8"), kAlice)->status, 200);
    ASSERT_EQ(c.request("GET", path("master.m3u8"), kBob)->status, 404);
    // Nothing is written on the request's path.
    EXPECT_TRUE(gw.views().empty());

    gw.advance(o.limits.view_interval);
    const auto views = gw.views();
    ASSERT_EQ(views.size(), 1U);
    EXPECT_EQ(views[0].video, *core::VideoId::parse(kVideo));
    EXPECT_EQ(views[0].viewer, *core::UserId::parse("alice"));
    EXPECT_NE(gw.metrics().find("view_events_recorded_total 1\n"), std::string::npos);
}

TEST_P(GatewayPlayback, ViewsBeyondTheBatchAreDroppedNotWaitedFor) {
    GatewayOptions o = options();
    o.manual_clock = true;
    o.limits.view_batch = 2;
    GatewayUnderTest gw(o);
    publish(gw);
    HttpClient c(gw.endpoint());
    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(c.request("GET", path("master.m3u8"), kAlice)->status, 200);
    }
    gw.advance(o.limits.view_interval);
    EXPECT_EQ(gw.views().size(), 2U);
    EXPECT_NE(gw.metrics().find("view_events_dropped_total 1\n"), std::string::npos);
}

TEST_P(GatewayPlayback, AFailedViewWriteCostsTheViewsNotThePlayback) {
    GatewayOptions o = options();
    o.manual_clock = true;
    GatewayUnderTest gw(o);
    publish(gw);
    gw.fail_views(core::ports::CatalogError::Unavailable);
    HttpClient c(gw.endpoint());
    ASSERT_EQ(c.request("GET", path("master.m3u8"), kAlice)->status, 200);
    gw.advance(o.limits.view_interval);
    const std::string m = gw.metrics();
    EXPECT_NE(m.find("view_batches_failed_total 1\n"), std::string::npos) << m;
    EXPECT_NE(m.find("view_events_dropped_total 1\n"), std::string::npos) << m;

    // The recorder carries on with the next batch once the log recovers.
    gw.fail_views(std::nullopt);
    ASSERT_EQ(c.request("GET", path("master.m3u8"), kAlice)->status, 200);
    gw.advance(o.limits.view_interval);
    EXPECT_EQ(gw.views().size(), 1U);
}

TEST_P(GatewayPlayback, ADrainWritesTheViewsItHolds) {
    GatewayUnderTest gw(options());
    publish(gw);
    HttpClient c(gw.endpoint());
    ASSERT_EQ(c.request("GET", path("master.m3u8"), kAlice)->status, 200);
    gw.drain();
    // The interval is 5 s; a drain does not wait for it.
    gw.on_loop([] {});
    gw.on_loop([] {});
    EXPECT_EQ(gw.views().size(), 1U);
}

TEST_P(GatewayPlayback, AViewFromARequestFinishingDuringADrainIsWrittenAtOnce) {
    GatewayOptions o = options();
    o.manual_clock = true;
    GatewayUnderTest gw(o);
    publish(gw);
    HttpClient c(gw.endpoint());
    // Held at authentication until the keys are refreshed, so it outlasts the drain's flush.
    ASSERT_TRUE(c.send_request("GET", path("master.m3u8"), "slow.alice", {}, {}));
    ASSERT_TRUE(ulw::test::eventually([&] { return gw.key_waiters() == 1; }));
    gw.drain();
    gw.refresh_keys();
    const auto r = c.read_response();
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200);
    // No time passes: the view must not wait out the batch interval.
    EXPECT_TRUE(ulw::test::eventually([&] { return gw.views().size() == 1; }));
}

INSTANTIATE_TEST_SUITE_P(Transports, GatewayPlayback,
                         ::testing::Values(gateway::Transport::Plain, gateway::Transport::Tls),
                         [](const ::testing::TestParamInfo<gateway::Transport>& p) {
                             return p.param == gateway::Transport::Tls ? "Tls" : "Plain";
                         });

} // namespace
