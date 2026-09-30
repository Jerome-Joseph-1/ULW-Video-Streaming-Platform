#include "gateway_harness.hpp"
#include "playback.hpp"
#include "send_window.hpp"
#include "support/eventually.hpp"
#include "support/http_client.hpp"
#include "support/socket_probe.hpp"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <format>
#include <gtest/gtest.h>
#include <optional>
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

// A media playlist of 1300 segments: about 130 KB once every segment URL is signed, many times
// what a small receive buffer holds.
std::string long_media_playlist() {
    std::string text = "#EXTM3U\n"
                       "#EXT-X-VERSION:7\n"
                       "#EXT-X-TARGETDURATION:4\n"
                       "#EXT-X-PLAYLIST-TYPE:VOD\n"
                       "#EXT-X-MAP:URI=\"init_0.mp4\"\n";
    for (int k = 0; k < 1300; ++k) {
        text += std::format("#EXTINF:4.000000,\nseg_{:05}.m4s\n", k);
    }
    text += "#EXT-X-ENDLIST\n";
    return text;
}

// A player's receive buffer, fixed small, which Linux doubles: the gateway's side of the
// connection sees its window shut after 16 KiB unread, and open again a read at a time.
constexpr int kPlayerReceiveBuffer = 8 * 1024;
// The segment size a client on an Ethernet path announces.
constexpr int kEthernetSegment = 1460;

// A player that frees its window a little at a time keeps its connection for as long as it
// reads. The kernel's own count of a shut window (TCP_USER_TIMEOUT) restarts only when the
// window opens wide enough for the whole unsent head of its queue, so it ended such a reader
// a fixed time after its window first shut, reading or not (ADR-0071): with that timeout at
// 1.5 s, this reader was reset about 2 s in. The gateway's socket has no user timeout, which
// is what guards the fix: the read below lasts seconds, not the 20 s the timeout was. The
// gateway's clock is held, so nothing but the kernel could end this connection meanwhile.
TEST_P(GatewayPlayback, APlayerReadingAPlaylistALittleAtATimeKeepsItsConnection) {
    GatewayOptions o = options();
    o.manual_clock = true;
    GatewayUnderTest gw(o);
    publish(gw);
    gw.put_object(key("720p/index.m3u8"), long_media_playlist());
    HttpClient c(gw.endpoint(), kPlayerReceiveBuffer);
    ASSERT_TRUE(c.connected());
    ASSERT_TRUE(c.send_request("GET", path("720p/index.m3u8"), kAlice));
    ASSERT_TRUE(c.readable());
    const auto client_port = ulw::test::tcp_port(c.fd(), false);
    ASSERT_TRUE(client_port);
    EXPECT_EQ(ulw::test::user_timeout_of(gw.port(), *client_port), 0);

    // 8 KiB, then a wall-clock pause of 250 ms, long enough for the gateway's side to probe
    // the shut window; the pause is a poll that ends early if the connection breaks.
    constexpr std::size_t kStep = std::size_t{8} * 1024;
    constexpr std::chrono::milliseconds kPause{250};
    const auto began = std::chrono::steady_clock::now();
    std::optional<ulw::test::HttpResponse> r;
    while (!r) {
        const auto n = c.read_some(kStep);
        ASSERT_TRUE(n) << "the connection ended after "
                       << std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - began)
                              .count()
                       << " ms, " << c.buffered() << " bytes read";
        r = c.take_response();
        if (!r) {
            ASSERT_FALSE(c.broken_within(kPause)) << "the connection broke";
        }
    }
    EXPECT_GE(std::chrono::steady_clock::now() - began, std::chrono::seconds(3));
    ASSERT_EQ(r->status, 200);
    EXPECT_GT(r->body.size(), std::size_t{100'000});
    EXPECT_TRUE(r->body.ends_with("#EXT-X-ENDLIST\n"));

    // Still open, and still served at once: the response had gone when the client asked again.
    const auto again = c.request("GET", "/healthz", "");
    ASSERT_TRUE(again);
    EXPECT_EQ(again->status, 200);
    EXPECT_EQ(gw.counters().timeouts_header, 0U);
}

// A connection the gateway gave up on while the kernel held all that was left of the response
// ends with a FIN, after the kernel has finished the response, as it did before the user
// timeout was cleared.
void expect_the_response_then_a_fin(HttpClient& c) {
    const auto end = c.read_to_end();
    ASSERT_TRUE(end) << "the connection was never closed";
    EXPECT_EQ(*end, 0);
    const auto r = c.take_response();
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    EXPECT_TRUE(r->body.ends_with("#EXT-X-ENDLIST\n"));
}

// The largest playlist the gateway serves: 16,000 segments, 512 KB stored and about 1.5 MB
// signed.
std::string largest_media_playlist() {
    std::string text = "#EXTM3U\n"
                       "#EXT-X-VERSION:7\n"
                       "#EXT-X-TARGETDURATION:4\n"
                       "#EXT-X-PLAYLIST-TYPE:VOD\n"
                       "#EXT-X-MAP:URI=\"init_0.mp4\"\n";
    for (int k = 0; k < 16'000; ++k) {
        text += std::format("#EXTINF:4.000000,\nseg_{:05}.m4s\n", k);
    }
    text += "#EXT-X-ENDLIST\n";
    return text;
}

// A player that stops reading is still ended by the header timeout, as an idle connection with
// its response unread. A response the gateway handed all of to the kernel is finished by it
// after a FIN, as before.
TEST_P(GatewayPlayback, APlayerThatStopsReadingIsClosedAtTheHeaderTimeout) {
    GatewayOptions o = options();
    o.manual_clock = true;
    GatewayUnderTest gw(o);
    publish(gw);
    gw.put_object(key("720p/index.m3u8"), long_media_playlist());
    HttpClient c(gw.endpoint(), kPlayerReceiveBuffer);
    ASSERT_TRUE(c.connected());
    ASSERT_TRUE(c.send_request("GET", path("720p/index.m3u8"), kAlice));
    ASSERT_TRUE(ulw::test::eventually([&] { return gw.counters().responses.at(1) == 1; }));
    // The kernel has taken the whole response (on io_uring, once the send completes).
    ASSERT_TRUE(ulw::test::eventually([&] { return gw.queued_output() == 0; }));

    gw.advance(o.limits.header_timeout);
    ASSERT_TRUE(ulw::test::eventually([&] { return gw.connections() == 0; }));
    EXPECT_EQ(gw.counters().timeouts_header, 1U);
    expect_the_response_then_a_fin(c);
}

// A response the gateway still held part of when it gave up on the connection is cut off
// either way, so the close is a reset, which drops what the kernel held too. The client
// announces the segment size of an ordinary network, which keeps the gateway's send buffer to
// tens of kilobytes, as it is on one; the largest playlist, 1.5 MB, is far more than that
// buffer and the client's window take.
TEST_P(GatewayPlayback, APlayerThatStopsReadingWithTheResponseStillInTheGatewayIsReset) {
    GatewayOptions o = options();
    o.manual_clock = true;
    GatewayUnderTest gw(o);
    publish(gw);
    gw.put_object(key("720p/index.m3u8"), largest_media_playlist());
    HttpClient c(gw.endpoint(), kPlayerReceiveBuffer, kEthernetSegment);
    ASSERT_TRUE(c.connected());
    ASSERT_TRUE(c.send_request("GET", path("720p/index.m3u8"), kAlice));
    ASSERT_TRUE(ulw::test::eventually([&] { return gw.counters().responses.at(1) == 1; }));
    ASSERT_GT(gw.queued_output(), 0U);

    gw.advance(o.limits.header_timeout);
    ASSERT_TRUE(ulw::test::eventually([&] { return gw.connections() == 0; }));
    EXPECT_EQ(gw.counters().timeouts_header, 1U);
    const auto end = c.read_to_end();
    ASSERT_TRUE(end) << "the connection was never closed";
    EXPECT_EQ(*end, ECONNRESET);
    EXPECT_FALSE(c.take_response());
}

// A drain that finds a connection just after a keep-alive response lingers on it and answers
// nothing more, even a request the client had already pipelined behind that response.
TEST_P(GatewayPlayback, ADrainJustAfterAResponseAnswersNoPipelinedRequest) {
    GatewayUnderTest gw(options());
    HttpClient c(gw.endpoint());
    ASSERT_TRUE(c.connected());
    // A create with a body the gateway refuses, sent as a token it has no key for yet, then a
    // probe behind it in the same write.
    const std::string body = "{}";
    const std::string first = "POST /api/v1/uploads HTTP/1.1\r\nHost: test\r\n"
                              "Authorization: Bearer slow.alice\r\n"
                              "Content-Length: " +
                              std::to_string(body.size()) + "\r\n\r\n" + body;
    ASSERT_TRUE(c.send_raw(first + "GET /healthz HTTP/1.1\r\nHost: test\r\n\r\n"));
    ASSERT_TRUE(ulw::test::eventually([&] { return gw.key_waiters() == 1; }));
    // The refusal is sent, and the drain begins, in one turn of the loop.
    gw.refresh_keys_then_drain();
    const auto end = c.read_to_end();
    ASSERT_TRUE(end) << "the connection was never closed";
    const auto r = c.take_response();
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_FALSE(c.take_response());
    // The client sees the FIN at once; the gateway lets the connection go when the linger
    // ends, 2 s on, having parsed nothing more. Where the response was still being sent when
    // the drain began (on io_uring, whose send completes in a later turn, and under TLS), the
    // connection lingers; otherwise it is closed at once.
    ASSERT_TRUE(
        ulw::test::eventually([&] { return gw.connections() == 0; }, std::chrono::seconds(5)));
    EXPECT_EQ(gw.counters().requests, 1U);
}

// A client that keeps asking and never reads is not read from while its last response is held
// back by its shut window, so each request no longer restarts the header timeout: it is
// closed a header timeout after the first response it left unread. Without that, requests
// 9 s apart held the connection open, and queued a playlist each, for 1000 requests.
TEST_P(GatewayPlayback, AClientThatAsksAndNeverReadsIsClosedAtTheHeaderTimeout) {
    GatewayOptions o = options();
    o.manual_clock = true;
    GatewayUnderTest gw(o);
    publish(gw);
    gw.put_object(key("720p/index.m3u8"), long_media_playlist());
    HttpClient c(gw.endpoint(), kPlayerReceiveBuffer);
    ASSERT_TRUE(c.connected());
    const core::Millis step = o.limits.header_timeout - core::Millis{1'000};
    std::uint64_t answered = 0;
    for (int k = 0; k < 8; ++k) {
        ASSERT_TRUE(c.send_request("GET", path("720p/index.m3u8"), kAlice));
        // An answer comes within milliseconds or, once the gateway has stopped reading, not
        // at all: a second is ample either way.
        if (!ulw::test::eventually([&] { return gw.counters().responses.at(1) > answered; },
                                   std::chrono::seconds(1))) {
            break;
        }
        ++answered;
        gw.advance(step);
    }
    EXPECT_EQ(answered, 1U);
    ASSERT_TRUE(ulw::test::eventually([&] { return gw.queued_output() == 0; }));
    gw.advance(step);
    ASSERT_TRUE(ulw::test::eventually([&] { return gw.connections() == 0; }));
    EXPECT_EQ(gw.counters().timeouts_header, 1U);
    expect_the_response_then_a_fin(c);
}

// Holds a client's writes back while set, so the ones made meanwhile leave in one segment.
void cork(HttpClient& c, bool on) {
    const int value = on ? 1 : 0;
    ASSERT_EQ(::setsockopt(c.fd(), IPPROTO_TCP, TCP_CORK, &value, sizeof value), 0);
}

// A request the parser turns away at its head leaves reading on, and the parser waiting for the
// loop to resume it. The next request, read before then in a receive of its own while the
// refusal is still on its way, is held back until the refusal has gone, and is then answered.
// The two requests reach the gateway in one segment, as a TLS record each, so the second is
// read right behind the refusal, and on io_uring the refusal's send completes a turn later.
// Only on io_uring over TLS does this reach that path: a plaintext receive carries both
// requests at once, and on epoll the refusal leaves at once. The next test holds a refusal
// back on epoll too. The gateway's clock is held, so only an answer, not the header timeout,
// ends the wait.
TEST_P(GatewayPlayback, ARequestHeldBackBehindARefusalFromTheParserIsAnswered) {
    GatewayOptions o = options();
    o.manual_clock = true;
    GatewayUnderTest gw(o);
    HttpClient c(gw.endpoint());
    ASSERT_TRUE(c.connected());
    // Answered first, so that under TLS the session tickets have gone before the refusal.
    const auto first = c.request("GET", "/healthz", "");
    ASSERT_TRUE(first);
    ASSERT_EQ(first->status, 200);

    cork(c, true);
    ASSERT_TRUE(c.send_raw("GET /no-such-route HTTP/1.1\r\nHost: test\r\n\r\n"));
    ASSERT_TRUE(c.send_raw("GET /healthz HTTP/1.1\r\nHost: test\r\n\r\n"));
    cork(c, false);
    const auto refused = c.read_response();
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->status, 404);
    ASSERT_TRUE(ulw::test::eventually([&] { return gw.counters().responses.at(1) == 2; }))
        << "the request behind the refusal was never answered";
    const auto answered = c.read_response();
    ASSERT_TRUE(answered);
    EXPECT_EQ(answered->status, 200);
    EXPECT_EQ(gw.counters().requests, 3U);
    EXPECT_EQ(gw.counters().timeouts_header, 0U);
}

// As above, with a request the parser kept from the refusal's own receive: it is answered
// first, then the one held behind it. On io_uring over TLS, the held request would otherwise be
// fed to the parser while it still waited to resume.
TEST_P(GatewayPlayback, ARequestTheParserKeptIsAnsweredBeforeTheOneHeldBehindIt) {
    GatewayOptions o = options();
    o.manual_clock = true;
    GatewayUnderTest gw(o);
    HttpClient c(gw.endpoint());
    ASSERT_TRUE(c.connected());
    const auto first = c.request("GET", "/healthz", "");
    ASSERT_TRUE(first);
    ASSERT_EQ(first->status, 200);

    cork(c, true);
    ASSERT_TRUE(c.send_raw("GET /no-such-route HTTP/1.1\r\nHost: test\r\n\r\n"
                           "GET /readyz HTTP/1.1\r\nHost: test\r\n\r\n"));
    ASSERT_TRUE(c.send_raw("GET /healthz HTTP/1.1\r\nHost: test\r\n\r\n"));
    cork(c, false);
    const auto refused = c.read_response();
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->status, 404);
    ASSERT_TRUE(ulw::test::eventually([&] { return gw.counters().responses.at(1) == 3; }))
        << "a request behind the refusal was never answered";
    const auto ready = c.read_response();
    ASSERT_TRUE(ready);
    EXPECT_EQ(ready->status, 200);
    EXPECT_EQ(ready->body, "ready\n");
    const auto ok = c.read_response();
    ASSERT_TRUE(ok);
    EXPECT_EQ(ok->status, 200);
    EXPECT_EQ(ok->body, "ok\n");
    EXPECT_EQ(gw.counters().requests, 4U);
    EXPECT_EQ(gw.counters().timeouts_header, 0U);
}

// A request held in the turn that refused the one before it, with the refusal held in the
// gateway's kernel by the client's shut window, ends the hold once the refusal has gone: it is
// answered, and so is the next request, and nothing is left held. Before, the resume the refusal
// had queued read the held request without ending the hold, so every later receive was held
// too, with reading left on and no bound. The client sends refusals it does not read until its
// window has no room for another, so the next one is read with nothing unsent and then waits.
// Under TLS the request behind it comes in a record of its own and is held, on either reactor;
// over plaintext the two arrive in one receive and the parser keeps the second.
TEST_P(GatewayPlayback, ARequestHeldInTheTurnOfARefusalEndsTheHold) {
    GatewayOptions o = options();
    o.manual_clock = true;
    GatewayUnderTest gw(o);
    HttpClient c(gw.endpoint(), kPlayerReceiveBuffer);
    ASSERT_TRUE(c.connected());
    const auto first = c.request("GET", "/healthz", "");
    ASSERT_TRUE(first);
    ASSERT_EQ(first->status, 200);
    const auto client_port = ulw::test::tcp_port(c.fd(), false);
    ASSERT_TRUE(client_port);
    const auto window = [&] { return ulw::test::send_window_of(gw.port(), *client_port); };
    // Everything answered so far has been sent and acknowledged.
    const auto settled = [&](std::uint64_t refusals) {
        const auto w = window();
        return gw.counters().responses.at(3) == refusals && gw.queued_output() == 0 && w &&
               w->unsent == 0 && w->in_flight == 0;
    };

    constexpr std::string_view kRefused = "GET /no-such-route HTTP/1.1\r\nHost: test\r\n\r\n";
    std::uint64_t refusals = 0;
    std::int64_t refusal_size = 0;
    for (;;) {
        const auto before = window();
        ASSERT_TRUE(before) << "the kernel does not report the send window";
        if (refusal_size > 0 && before->room < refusal_size) {
            break;
        }
        ASSERT_LT(refusals, 1000U) << "the client's window never filled";
        ASSERT_TRUE(c.send_raw(kRefused));
        ++refusals;
        ASSERT_TRUE(ulw::test::eventually([&] { return settled(refusals); }))
            << "refusal " << refusals << " did not leave in full";
        refusal_size = static_cast<std::int64_t>(window()->sent - before->sent);
    }

    cork(c, true);
    ASSERT_TRUE(c.send_raw(kRefused));
    ASSERT_TRUE(c.send_raw("GET /healthz HTTP/1.1\r\nHost: test\r\n\r\n"));
    cork(c, false);
    ++refusals;
    ASSERT_TRUE(ulw::test::eventually([&] { return gw.counters().responses.at(3) == refusals; }));
    if (GetParam() == gateway::Transport::Tls) {
        ASSERT_TRUE(ulw::test::eventually([&] { return gw.held_bytes() > 0; }));
    }
    ASSERT_GT(window()->unsent, 0U);

    // The client reads everything, which lets the refusal go; the next drain check sees it.
    for (std::uint64_t k = 0; k < refusals; ++k) {
        const auto r = c.read_response();
        ASSERT_TRUE(r) << "refusal " << k;
        ASSERT_EQ(r->status, 404);
    }
    ASSERT_TRUE(ulw::test::eventually([&] { return window()->unsent == 0; }));
    gw.advance(core::Millis{100});
    const auto held = c.read_response();
    ASSERT_TRUE(held);
    EXPECT_EQ(held->status, 200);

    ASSERT_TRUE(c.send_request("GET", "/healthz", ""));
    ASSERT_TRUE(ulw::test::eventually([&] { return gw.counters().responses.at(1) == 3; }))
        << "the request after the hold was never answered; " << gw.held_bytes() << " bytes held";
    const auto next = c.read_response();
    ASSERT_TRUE(next);
    EXPECT_EQ(next->status, 200);
    EXPECT_EQ(gw.held_bytes(), 0U);
    EXPECT_EQ(gw.counters().timeouts_header, 0U);
}

// A client still reading its last response when the drain begins is given the linger to
// finish it, not closed at once, and one that has finished reading is let go.
TEST_P(GatewayPlayback, ADrainLetsAPlayerFinishReadingItsLastResponse) {
    GatewayUnderTest gw(options());
    publish(gw);
    gw.put_object(key("720p/index.m3u8"), long_media_playlist());
    HttpClient c(gw.endpoint(), kPlayerReceiveBuffer);
    ASSERT_TRUE(c.connected());
    ASSERT_TRUE(c.send_request("GET", path("720p/index.m3u8"), kAlice));
    ASSERT_TRUE(ulw::test::eventually([&] { return gw.counters().responses.at(1) == 1; }));
    gw.drain();
    // Read as fast as it comes: the whole response, then the gateway's FIN.
    std::optional<ulw::test::HttpResponse> r;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!r && std::chrono::steady_clock::now() < deadline) {
        ASSERT_TRUE(c.read_some(std::size_t{64} * 1024)) << c.buffered() << " bytes read";
        r = c.take_response();
        if (!r) {
            c.readable(std::chrono::milliseconds(100));
        }
    }
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    EXPECT_TRUE(r->body.ends_with("#EXT-X-ENDLIST\n"));
    EXPECT_TRUE(c.closed_by_peer());
    EXPECT_TRUE(ulw::test::eventually([&] { return gw.finished(); }));
}

INSTANTIATE_TEST_SUITE_P(Transports, GatewayPlayback,
                         ::testing::Values(gateway::Transport::Plain, gateway::Transport::Tls),
                         [](const ::testing::TestParamInfo<gateway::Transport>& p) {
                             return p.param == gateway::Transport::Tls ? "Tls" : "Plain";
                         });

} // namespace
