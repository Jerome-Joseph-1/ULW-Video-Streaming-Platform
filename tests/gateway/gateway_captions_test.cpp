#include "gateway_harness.hpp"
#include "support/http_client.hpp"

#include <cstddef>
#include <gtest/gtest.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// A video's captions over HTTP (ADR-0139): the owner's and the service's PUT and DELETE, the
// master listing them, the subtitles playlist, and the same access rules as the video's own.
namespace {

using ulw::test::Backend;
using ulw::test::GatewayOptions;
using ulw::test::GatewayUnderTest;
using ulw::test::HttpClient;
using ulw::test::HttpResponse;

constexpr std::string_view kAlice = "user.alice";
constexpr std::string_view kBob = "user.bob";
constexpr std::string_view kService = "service.backend";
constexpr std::string_view kVideo = "01890a5d-ac96-774b-bcce-b302099a8057";
constexpr std::string_view kMissing = "01890a5d-ac96-774b-bcce-b302099a8058";

constexpr std::string_view kMaster = "#EXTM3U\n"
                                     "#EXT-X-VERSION:7\n"
                                     "#EXT-X-STREAM-INF:BANDWIDTH=1020800,RESOLUTION=1280x720,"
                                     "CODECS=\"avc1.4d401f,mp4a.40.2\"\n"
                                     "720p/index.m3u8\n";

constexpr std::string_view kVtt = "WEBVTT\n"
                                  "\n"
                                  "00:00.000 --> 00:02.000\n"
                                  "Hello.\n";

std::string video_path(std::string_view rest = {}, std::string_view video = kVideo) {
    std::string out = "/api/v1/videos/" + std::string(video);
    if (!rest.empty()) {
        out += "/" + std::string(rest);
    }
    return out;
}

std::string caption_path(std::string_view language = "en", std::string_view video = kVideo) {
    return video_path("captions/" + std::string(language), video);
}

std::string service_caption_path(std::string_view language = "en",
                                 std::string_view video = kVideo) {
    return "/api/v1/service/videos/" + std::string(video) + "/captions/" + std::string(language);
}

std::string playlist_path(std::string_view language = "en") {
    return caption_path(language) + "/index.m3u8";
}

std::string key(std::string_view rest) {
    return "videos/" + std::string(kVideo) + "/hls/" + std::string(rest);
}

core::VideoRecord video(core::VideoState state, core::Visibility visibility = {}) {
    const bool ready = state == core::VideoState::Ready;
    return core::VideoRecord{.id = *core::VideoId::parse(kVideo),
                             .owner = *core::UserId::parse("alice"),
                             .title = "trip",
                             .state = state,
                             .version = 3,
                             .error_reason = std::nullopt,
                             .duration = ready ? std::optional(core::Millis{5'250}) : std::nullopt,
                             .visibility = visibility};
}

class GatewayCaptions : public ::testing::Test {
protected:
    [[nodiscard]] static GatewayOptions options() {
        GatewayOptions o;
        o.backend = Backend::Fake;
        return o;
    }

    void publish(core::VideoState state = core::VideoState::Ready,
                 core::Visibility visibility = {}) {
        gw_.put_video(video(state, visibility));
        gw_.put_object(key("master.m3u8"), kMaster);
    }

    std::optional<HttpResponse> put(std::string_view path, std::string_view body,
                                    std::string_view token = kAlice) {
        return client_.request("PUT", path, token, std::as_bytes(std::span(body)),
                               {{"Content-Type", "text/vtt"}});
    }

    std::optional<HttpResponse> call(std::string_view method, std::string_view path,
                                     std::string_view token = kAlice) {
        return client_.request(method, path, token);
    }

    // The stored file, or nullopt when there is none.
    std::optional<std::string> stored(std::string_view language = "en") {
        const auto bytes = gw_.reader().fetch_small(
            *core::StorageKey::parse(key("captions/" + std::string(language) + ".vtt")), 1 << 20);
        if (!bytes) {
            return std::nullopt;
        }
        std::string text(bytes->size(), '\0');
        for (std::size_t i = 0; i < bytes->size(); ++i) {
            text[i] = static_cast<char>((*bytes)[i]);
        }
        return text;
    }

    GatewayUnderTest gw_{options()};
    HttpClient client_{gw_.endpoint()};
};

TEST_F(GatewayCaptions, TheOwnersFileIsStoredAndTheMasterListsIt) {
    publish();
    const auto r = put(caption_path("EN-us"), kVtt);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 204) << r->body;
    // Stored next to the renditions, under the language's lowercase form.
    EXPECT_EQ(stored("en-us"), std::string(kVtt));

    const auto master = call("GET", video_path("master.m3u8"));
    ASSERT_TRUE(master);
    ASSERT_EQ(master->status, 200) << master->body;
    EXPECT_EQ(master->body,
              "#EXTM3U\n"
              "#EXT-X-VERSION:7\n"
              "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"subs\",NAME=\"en-us\",LANGUAGE=\"en-us\","
              "DEFAULT=NO,AUTOSELECT=YES,URI=\"" +
                  playlist_path("en-us") +
                  "\"\n"
                  "#EXT-X-STREAM-INF:BANDWIDTH=1020800,RESOLUTION=1280x720,"
                  "CODECS=\"avc1.4d401f,mp4a.40.2\",SUBTITLES=\"subs\"\n" +
                  video_path("720p/index.m3u8") + "\n");

    const auto playlist = call("GET", playlist_path("en-us"));
    ASSERT_TRUE(playlist);
    ASSERT_EQ(playlist->status, 200) << playlist->body;
    EXPECT_EQ(playlist->header("content-type"), "application/vnd.apple.mpegurl");
    EXPECT_EQ(playlist->header("cache-control"), "private, max-age=60");
    EXPECT_EQ(playlist->body, "#EXTM3U\n"
                              "#EXT-X-VERSION:3\n"
                              "#EXT-X-TARGETDURATION:6\n"
                              "#EXT-X-MEDIA-SEQUENCE:0\n"
                              "#EXT-X-PLAYLIST-TYPE:VOD\n"
                              "#EXTINF:5.250,\n"
                              "fake://" +
                                  key("captions/en-us.vtt") +
                                  "\n"
                                  "#EXT-X-ENDLIST\n");
    // Any spelling of the tag is the same language.
    const auto upper = call("GET", playlist_path("EN-US"));
    ASSERT_TRUE(upper);
    EXPECT_EQ(upper->status, 200);

    const auto c = gw_.counters();
    EXPECT_EQ(c.captions_stored, 1U);
    EXPECT_EQ(c.playlists_captions, 2U);
    const std::string metrics = gw_.metrics();
    EXPECT_NE(metrics.find(R"(caption_changes_total{op="stored"} 1)"), std::string::npos);
    EXPECT_NE(metrics.find(R"(playlist_requests_total{kind="captions"} 2)"), std::string::npos);
}

TEST_F(GatewayCaptions, APutReplacesTheFileAndEachLanguageIsListedOnce) {
    publish();
    ASSERT_EQ(put(caption_path("fr"), kVtt)->status, 204);
    ASSERT_EQ(put(caption_path("en"), kVtt)->status, 204);
    const std::string newer = std::string(kVtt) + "\n00:02.000 --> 00:03.000\nAgain.\n";
    ASSERT_EQ(put(caption_path("en"), newer)->status, 204);
    EXPECT_EQ(stored("en"), newer);
    const auto master = call("GET", video_path("master.m3u8"));
    ASSERT_TRUE(master);
    ASSERT_EQ(master->status, 200);
    // In language order, each once.
    const std::size_t en = master->body.find("NAME=\"en\"");
    const std::size_t fr = master->body.find("NAME=\"fr\"");
    EXPECT_NE(en, std::string::npos) << master->body;
    EXPECT_LT(en, fr) << master->body;
    EXPECT_EQ(master->body.find("NAME=\"en\"", en + 1), std::string::npos) << master->body;
}

TEST_F(GatewayCaptions, ABadFileIsRefusedWithWhatAndWhere) {
    publish();
    const auto r = put(caption_path(), "WEBVTT\n\n00:01.000 --> 00:00.500\nbackwards\n");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(r->body, R"({"error":"bad_captions","reason":"timing","line":3})");
    const auto text = put(caption_path(), "1\n00:01.000 --> 00:02.000\nno signature\n");
    ASSERT_TRUE(text);
    EXPECT_EQ(text->body, R"({"error":"bad_captions","reason":"header","line":1})");
    EXPECT_EQ(gw_.counters().captions_rejected, 2U);
    EXPECT_EQ(stored(), std::nullopt);
    // Nothing is listed.
    const auto playlist = call("GET", playlist_path());
    ASSERT_TRUE(playlist);
    EXPECT_EQ(playlist->status, 404);
}

TEST_F(GatewayCaptions, TheBodyMustBeThereAndWithinItsBound) {
    publish();
    const auto empty = call("PUT", caption_path());
    ASSERT_TRUE(empty);
    EXPECT_EQ(empty->status, 400);
    HttpClient big(gw_.endpoint());
    const std::string huge = std::string(kVtt) + std::string(std::size_t{512} * 1024, 'a');
    const auto r = big.request("PUT", caption_path(), kAlice, std::as_bytes(std::span(huge)));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 413);
    EXPECT_EQ(stored(), std::nullopt);
}

TEST_F(GatewayCaptions, ThePathNamesAVideoAndALanguage) {
    publish();
    const auto language = put(caption_path("english"), kVtt);
    ASSERT_TRUE(language);
    EXPECT_EQ(language->status, 400);
    EXPECT_EQ(language->body, R"({"error":"bad_language"})");
    const auto id = put(video_path("captions/en", "not-a-uuid"), kVtt);
    ASSERT_TRUE(id);
    EXPECT_EQ(id->status, 404);
    EXPECT_EQ(id->body, R"({"error":"not_found"})");
    const auto missing = put(caption_path("en", kMissing), kVtt);
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 404);
    EXPECT_EQ(missing->body, R"({"error":"not_found"})");
    const auto bad_delete = call("DELETE", caption_path("x"));
    ASSERT_TRUE(bad_delete);
    EXPECT_EQ(bad_delete->status, 400);
    // The route takes PUT and DELETE only.
    const auto get = call("GET", caption_path());
    ASSERT_TRUE(get);
    EXPECT_EQ(get->status, 405);
}

TEST_F(GatewayCaptions, OnlyTheOwnerChangesThem) {
    // Bob may see an unlisted video, and so learns it exists: 403. A private one is 404 to him.
    publish(core::VideoState::Ready, core::Visibility::unlisted());
    const auto viewer = put(caption_path(), kVtt, kBob);
    ASSERT_TRUE(viewer);
    EXPECT_EQ(viewer->status, 403);
    EXPECT_EQ(viewer->body, R"({"error":"forbidden"})");
    const auto viewer_delete = call("DELETE", caption_path(), kBob);
    ASSERT_TRUE(viewer_delete);
    EXPECT_EQ(viewer_delete->status, 403);

    ASSERT_EQ(put(caption_path(), kVtt)->status, 204);
    // He plays them as he plays the video.
    const auto playlist = call("GET", playlist_path(), kBob);
    ASSERT_TRUE(playlist);
    EXPECT_EQ(playlist->status, 200) << playlist->body;

    publish(core::VideoState::Ready);
    const auto stranger = put(caption_path(), kVtt, kBob);
    ASSERT_TRUE(stranger);
    EXPECT_EQ(stranger->status, 404);
    EXPECT_EQ(stranger->body, R"({"error":"not_found"})");
    const auto hidden = call("GET", playlist_path(), kBob);
    ASSERT_TRUE(hidden);
    EXPECT_EQ(hidden->status, 404);
    EXPECT_TRUE(hidden->body.empty());
}

TEST_F(GatewayCaptions, APublicVideosCaptionsPlayWithoutAToken) {
    publish(core::VideoState::Ready, core::Visibility::anyone());
    ASSERT_EQ(put(caption_path(), kVtt)->status, 204);
    const auto anonymous = call("GET", playlist_path(), {});
    ASSERT_TRUE(anonymous);
    EXPECT_EQ(anonymous->status, 200) << anonymous->body;
    EXPECT_NE(anonymous->body.find("fake://" + key("captions/en.vtt")), std::string::npos);
    // Changing them always takes a token.
    const auto put_anonymously = put(caption_path(), kVtt, {});
    ASSERT_TRUE(put_anonymously);
    EXPECT_EQ(put_anonymously->status, 401);

    // Refused at its head, with its body unread, that request closed the connection.
    publish(core::VideoState::Ready);
    HttpClient again(gw_.endpoint());
    const auto hidden = again.request("GET", playlist_path(), {});
    ASSERT_TRUE(hidden);
    EXPECT_EQ(hidden->status, 401);
}

TEST_F(GatewayCaptions, CaptionsMayComeBeforeTheVideoIsReady) {
    publish(core::VideoState::Processing);
    ASSERT_EQ(put(caption_path(), kVtt)->status, 204);
    // The playlists wait for the video.
    const auto early = call("GET", playlist_path());
    ASSERT_TRUE(early);
    EXPECT_EQ(early->status, 409);
    publish(core::VideoState::Ready);
    const auto ready = call("GET", playlist_path());
    ASSERT_TRUE(ready);
    EXPECT_EQ(ready->status, 200);
    // A language not put is not there.
    const auto other = call("GET", playlist_path("de"));
    ASSERT_TRUE(other);
    EXPECT_EQ(other->status, 404);
    const auto malformed = call("GET", playlist_path("x"));
    ASSERT_TRUE(malformed);
    EXPECT_EQ(malformed->status, 404);
}

TEST_F(GatewayCaptions, ADeleteUnlistsAndRemovesTheFile) {
    publish();
    ASSERT_EQ(put(caption_path(), kVtt)->status, 204);
    const auto r = call("DELETE", caption_path());
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 204) << r->body;
    EXPECT_EQ(stored(), std::nullopt);
    const auto master = call("GET", video_path("master.m3u8"));
    ASSERT_TRUE(master);
    EXPECT_EQ(master->body.find("SUBTITLES"), std::string::npos) << master->body;
    const auto playlist = call("GET", playlist_path());
    ASSERT_TRUE(playlist);
    EXPECT_EQ(playlist->status, 404);
    // Again, and for a language never put: the same.
    const auto again = call("DELETE", caption_path());
    ASSERT_TRUE(again);
    EXPECT_EQ(again->status, 204);
    EXPECT_EQ(gw_.counters().captions_removed, 2U);
}

TEST_F(GatewayCaptions, ADeletedVideoHasNoCaptionsToChange) {
    publish();
    ASSERT_EQ(put(caption_path(), kVtt)->status, 204);
    ASSERT_EQ(call("DELETE", video_path())->status, 204);
    const auto r = put(caption_path(), kVtt);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 404);
    const auto d = call("DELETE", caption_path());
    ASSERT_TRUE(d);
    EXPECT_EQ(d->status, 404);
}

TEST_F(GatewayCaptions, TheServicePutsAndDeletesAnyVideos) {
    publish();
    const auto r = put(service_caption_path(), kVtt, kService);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 204) << r->body;
    EXPECT_EQ(stored(), std::string(kVtt));
    const auto d = call("DELETE", service_caption_path(), kService);
    ASSERT_TRUE(d);
    EXPECT_EQ(d->status, 204) << d->body;
    EXPECT_EQ(stored(), std::nullopt);
    // Nothing is written for a video that does not exist.
    const auto missing = put(service_caption_path("en", kMissing), kVtt, kService);
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 404);
    EXPECT_EQ(missing->body, R"({"error":"not_found"})");
    // The owner is not the service.
    const auto owner = put(service_caption_path(), kVtt, kAlice);
    ASSERT_TRUE(owner);
    EXPECT_EQ(owner->status, 403);
    EXPECT_EQ(owner->body, R"({"error":"forbidden"})");
    const auto owner_delete = call("DELETE", service_caption_path(), kAlice);
    ASSERT_TRUE(owner_delete);
    EXPECT_EQ(owner_delete->status, 403);
}

TEST_F(GatewayCaptions, AStoreOrCatalogOutageIsAskedAgain) {
    publish();
    ASSERT_EQ(put(caption_path(), kVtt)->status, 204);
    gw_.set_plan({.fail_admin = core::ports::StorageError::Transient});
    const auto r = put(caption_path("fr"), kVtt);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 503);
    EXPECT_EQ(r->header("retry-after"), "5");
    // The language stays unlisted while its file could not be removed: the delete is retried.
    const auto d = call("DELETE", caption_path());
    ASSERT_TRUE(d);
    EXPECT_EQ(d->status, 503);
    gw_.set_plan({});
    const auto fr = call("GET", playlist_path("fr"));
    ASSERT_TRUE(fr);
    EXPECT_EQ(fr->status, 404);
    ASSERT_EQ(call("DELETE", caption_path())->status, 204);
    EXPECT_EQ(stored(), std::nullopt);

    gw_.fail_catalog(core::ports::CatalogError::Unavailable);
    const auto master = call("GET", video_path("master.m3u8"));
    ASSERT_TRUE(master);
    EXPECT_EQ(master->status, 503);
    const auto put_down = put(caption_path(), kVtt);
    ASSERT_TRUE(put_down);
    EXPECT_EQ(put_down->status, 503);
    gw_.fail_catalog(std::nullopt);
}

TEST(GatewayCaptionsLimits, AFileHoldsOneOfTheUsersUploadSlots) {
    GatewayOptions o;
    o.backend = Backend::Fake;
    o.limits.max_uploads_per_user = 0;
    GatewayUnderTest gw(o);
    gw.put_video(video(core::VideoState::Ready));
    HttpClient c(gw.endpoint());
    const auto r = c.request("PUT", caption_path(), kAlice, std::as_bytes(std::span(kVtt)));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 429);

    o.limits.max_upload_slots = 0;
    GatewayUnderTest full(o);
    full.put_video(video(core::VideoState::Ready));
    HttpClient f(full.endpoint());
    const auto busy = f.request("PUT", caption_path(), kAlice, std::as_bytes(std::span(kVtt)));
    ASSERT_TRUE(busy);
    EXPECT_EQ(busy->status, 503);
}

} // namespace
