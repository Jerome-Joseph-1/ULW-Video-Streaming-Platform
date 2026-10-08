#include "gateway_harness.hpp"
#include "support/http_client.hpp"

#include <cstddef>
#include <format>
#include <gtest/gtest.h>
#include <initializer_list>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The operator's backend's control of VOD (ADR-0100), over HTTP: who may upload, the owner's
// deletion, and the service API's visibility, takedown and listing.
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
constexpr std::string_view kRoom = "0192f3c4-7a1b-7c2d-8e3f-0123456789ab";

constexpr std::string_view kMaster = "#EXTM3U\n"
                                     "#EXT-X-VERSION:7\n"
                                     "#EXT-X-STREAM-INF:BANDWIDTH=1020800,RESOLUTION=1280x720,"
                                     "CODECS=\"avc1.4d401f,mp4a.40.2\"\n"
                                     "720p/index.m3u8\n";

constexpr std::string_view kMedia = "#EXTM3U\n"
                                    "#EXT-X-VERSION:7\n"
                                    "#EXT-X-TARGETDURATION:4\n"
                                    "#EXT-X-PLAYLIST-TYPE:VOD\n"
                                    "#EXT-X-MAP:URI=\"init_0.mp4\"\n"
                                    "#EXTINF:4.000000,\n"
                                    "seg_00000.m4s\n"
                                    "#EXT-X-ENDLIST\n";

std::string video_path(std::string_view rest = {}, std::string_view video = kVideo) {
    std::string out = "/api/v1/videos/" + std::string(video);
    if (!rest.empty()) {
        out += "/" + std::string(rest);
    }
    return out;
}

std::string service_path(std::string_view video = kVideo) {
    return "/api/v1/service/videos/" + std::string(video);
}

std::string grants_path(std::string_view user = {}) {
    std::string out = service_path() + "/grants";
    if (!user.empty()) {
        out += "/" + std::string(user);
    }
    return out;
}

std::string key(std::string_view rest) {
    return "videos/" + std::string(kVideo) + "/hls/" + std::string(rest);
}

core::VideoRecord video(core::VideoState state, core::Visibility visibility = {},
                        std::string_view id = kVideo, std::string_view owner = "alice") {
    const bool ready = state == core::VideoState::Ready;
    return core::VideoRecord{.id = *core::VideoId::parse(id),
                             .owner = *core::UserId::parse(owner),
                             .title = "trip",
                             .state = state,
                             .version = 3,
                             .error_reason = state == core::VideoState::Failed
                                                 ? std::optional<std::string>("bad")
                                                 : std::nullopt,
                             .duration = ready ? std::optional(core::Millis{5'000}) : std::nullopt,
                             .visibility = visibility};
}

// `n` video ids that sort in the order they are made.
std::string nth_video(int n) {
    return std::format("01890a5d-ac96-774b-bcce-b302099a{:04}", n);
}

class GatewayVodControl : public ::testing::Test {
protected:
    [[nodiscard]] static GatewayOptions options() {
        GatewayOptions o;
        o.backend = Backend::Fake;
        return o;
    }

    void publish(core::VideoState state, core::Visibility visibility = {}) {
        gw_.put_video(video(state, visibility));
        gw_.put_object(key("master.m3u8"), kMaster);
        gw_.put_object(key("720p/index.m3u8"), kMedia);
    }

    std::optional<HttpResponse> get(std::string_view path, std::string_view token) {
        return client_.request("GET", path, token);
    }

    std::optional<HttpResponse> remove(std::string_view token, std::string_view video = kVideo) {
        return client_.request("DELETE", video_path({}, video), token);
    }

    std::optional<HttpResponse> service(std::string_view method, std::string_view path,
                                        std::string_view token = kService) {
        return client_.request(method, path, token);
    }

    std::optional<HttpResponse> service_patch(std::string_view body,
                                              std::string_view video = kVideo,
                                              std::string_view token = kService) {
        return client_.request("PATCH", service_path(video), token, std::as_bytes(std::span(body)),
                               {{"Content-Type", "application/json"}});
    }

    std::optional<HttpResponse> create_upload(std::string_view token) {
        const std::string body =
            R"({"filename":"a.mp4","size_bytes":10,"content_type":"video/mp4"})";
        return client_.request("POST", "/api/v1/uploads", token, std::as_bytes(std::span(body)),
                               {{"Content-Type", "application/json"}});
    }

    // What every path that serves the video answers `token`: metadata and both playlists.
    void expect_gone_for(std::string_view token) {
        for (const std::string& path :
             {video_path(), video_path("master.m3u8"), video_path("720p/index.m3u8")}) {
            const auto r = get(path, token);
            ASSERT_TRUE(r) << path;
            EXPECT_EQ(r->status, 404) << token << " " << path;
            EXPECT_TRUE(r->body.empty()) << r->body;
        }
    }

    void expect_playable_by(std::string_view token) {
        for (const std::string& path :
             {video_path(), video_path("master.m3u8"), video_path("720p/index.m3u8")}) {
            const auto r = get(path, token);
            ASSERT_TRUE(r) << path;
            EXPECT_EQ(r->status, 200) << token << " " << path << " " << r->body;
        }
    }

    GatewayUnderTest gw_{options()};
    HttpClient client_{gw_.endpoint()};
};

// Who may upload: the deployment's claim, which the fake verifier leaves out of "watcher."
// tokens (ULW_UPLOADER_SCOPE set); everyone else carries it, as every token does when it is
// unset.
TEST_F(GatewayVodControl, AUserWithoutTheUploaderClaimMayNotCreateAnUpload) {
    const auto refused = create_upload("watcher.alice");
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->status, 403);
    EXPECT_EQ(refused->body, R"({"error":"upload_not_allowed"})");
    EXPECT_TRUE(gw_.jobs().empty());
    const auto created = create_upload(kAlice);
    ASSERT_TRUE(created);
    EXPECT_EQ(created->status, 201) << created->body;
}

TEST_F(GatewayVodControl, AUserWithoutTheUploaderClaimStillWatchesAndManagesTheirVideos) {
    publish(core::VideoState::Ready);
    expect_playable_by("watcher.alice");
    const auto deleted = remove("watcher.alice");
    ASSERT_TRUE(deleted);
    EXPECT_EQ(deleted->status, 204);
}

TEST_F(GatewayVodControl, TheOwnerDeletesAVideoAndItIsGoneFromEveryRead) {
    publish(core::VideoState::Ready, core::Visibility::unlisted());
    ASSERT_EQ(service("POST", grants_path("carol"))->status, 204);
    expect_playable_by(kBob);

    const auto deleted = remove(kAlice);
    ASSERT_TRUE(deleted);
    EXPECT_EQ(deleted->status, 204);
    EXPECT_TRUE(deleted->body.empty());
    // Gone for its owner, for anyone it was shared with, and for the grant's holder.
    for (const std::string_view token : {kAlice, kBob, std::string_view("user.carol")}) {
        expect_gone_for(token);
    }
    // Its grants are gone with it, and no new one can be made.
    for (const auto& [method, path] :
         std::initializer_list<std::pair<std::string_view, std::string>>{
             {"GET", grants_path()},
             {"POST", grants_path("bob")},
             {"DELETE", grants_path("carol")}}) {
        const auto r = service(method, path);
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 404) << method << " " << path;
        EXPECT_EQ(r->body, R"({"error":"not_found"})");
    }
    // Nobody may change it any more.
    const std::string body = R"({"visibility":"private"})";
    EXPECT_EQ(client_
                  .request("PATCH", video_path(), kAlice, std::as_bytes(std::span(body)),
                           {{"Content-Type", "application/json"}})
                  ->status,
              404);
    EXPECT_EQ(service_patch(body)->status, 404);
    // Its objects wait for the reaper.
    EXPECT_EQ(gw_.purges(), std::vector<core::VideoId>{*core::VideoId::parse(kVideo)});
}

TEST_F(GatewayVodControl, DeletingAgainChangesNothing) {
    publish(core::VideoState::Ready);
    ASSERT_EQ(remove(kAlice)->status, 204);
    const auto again = remove(kAlice);
    ASSERT_TRUE(again);
    EXPECT_EQ(again->status, 204);
    EXPECT_EQ(gw_.purges().size(), 1U);
    // Someone else still learns nothing.
    EXPECT_EQ(remove(kBob)->status, 404);
}

TEST_F(GatewayVodControl, EveryCommittedStateMayBeDeleted) {
    int n = 0;
    for (const core::VideoState state :
         {core::VideoState::Processing, core::VideoState::Ready, core::VideoState::Failed}) {
        const std::string id = nth_video(++n);
        gw_.put_video(video(state, {}, id));
        const auto r = remove(kAlice, id);
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 204) << id;
        EXPECT_EQ(get(video_path({}, id), kAlice)->status, 404);
    }
    EXPECT_EQ(gw_.purges().size(), 3U);
}

TEST_F(GatewayVodControl, AnUploadInProgressIsCancelledNotDeleted) {
    for (const core::VideoState state : {core::VideoState::Init, core::VideoState::Uploading}) {
        publish(state);
        const auto r = remove(kAlice);
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 409);
        EXPECT_EQ(r->body, R"({"error":"not_committed"})");
        EXPECT_EQ(get(video_path(), kAlice)->status, 200);
        // Nobody else learns it exists.
        EXPECT_EQ(remove(kBob)->status, 404);
    }
    EXPECT_TRUE(gw_.purges().empty());
}

TEST_F(GatewayVodControl, OnlyTheOwnerDeletesAndOthersCannotTellTheVideoExists) {
    publish(core::VideoState::Ready);
    for (const std::string_view video : {kVideo, kMissing, std::string_view("not-a-uuid")}) {
        const auto r = remove(kBob, video);
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 404) << video;
        EXPECT_EQ(r->body, R"({"error":"not_found"})");
    }
    // A viewer knows it exists, and is told they may not.
    publish(core::VideoState::Ready, core::Visibility::unlisted());
    const auto viewer = remove(kBob);
    ASSERT_TRUE(viewer);
    EXPECT_EQ(viewer->status, 403);
    EXPECT_EQ(viewer->body, R"({"error":"forbidden"})");
    // The service is not the owner either: it has its own route.
    EXPECT_EQ(remove(kService)->status, 403);
    expect_playable_by(kAlice);
    EXPECT_TRUE(gw_.purges().empty());
}

TEST_F(GatewayVodControl, ACookieDeleteNeedsAnAllowedPage) {
    publish(core::VideoState::Ready);
    const std::map<std::string, std::string> cookie{{"Cookie", "auth_token=user.alice"}};
    EXPECT_EQ(client_.request("DELETE", video_path(), "", {}, cookie)->status, 403);
    expect_playable_by(kAlice);
    HttpClient other(gw_.endpoint());
    auto allowed = cookie;
    allowed["Origin"] = std::string(ulw::test::kAllowedOrigin);
    EXPECT_EQ(other.request("DELETE", video_path(), "", {}, allowed)->status, 204);
}

TEST_F(GatewayVodControl, ADeleteCarriesNoBody) {
    publish(core::VideoState::Ready);
    const std::string body = "{}";
    EXPECT_EQ(
        client_.request("DELETE", video_path(), kAlice, std::as_bytes(std::span(body)))->status,
        400);
    HttpClient other(gw_.endpoint());
    EXPECT_EQ(
        other.request("DELETE", service_path(), kService, std::as_bytes(std::span(body)))->status,
        400);
    // Each refusal closed its connection; the video is still there.
    HttpClient third(gw_.endpoint());
    EXPECT_EQ(third.request("GET", video_path("master.m3u8"), kAlice)->status, 200);
    EXPECT_TRUE(gw_.purges().empty());
}

TEST_F(GatewayVodControl, TheServiceSetsAnyVideosVisibility) {
    publish(core::VideoState::Ready);
    const auto unlisted = service_patch(R"({"visibility":"unlisted"})");
    ASSERT_TRUE(unlisted);
    ASSERT_EQ(unlisted->status, 200) << unlisted->body;
    EXPECT_EQ(unlisted->header("content-type"), "application/json");
    EXPECT_NE(unlisted->body.find(R"("visibility":"unlisted")"), std::string::npos);
    expect_playable_by(kBob);

    // A room must list the video's owner, as on the owner's PATCH.
    const std::string room = R"({"visibility":"room:)" + std::string(kRoom) + "\"}";
    const auto refused = service_patch(room);
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->status, 403);
    EXPECT_EQ(refused->body, R"({"error":"not_member"})");
    gw_.add_member(kRoom, "alice");
    const auto shared = service_patch(room);
    ASSERT_TRUE(shared);
    EXPECT_EQ(shared->status, 200) << shared->body;
    EXPECT_EQ(get(video_path(), kBob)->status, 404);
    gw_.add_member(kRoom, "bob");
    expect_playable_by(kBob);

    ASSERT_EQ(service_patch(R"({"visibility":"private"})")->status, 200);
    EXPECT_EQ(get(video_path(), kBob)->status, 404);
}

TEST_F(GatewayVodControl, TheServicesVisibilityIsCheckedAsTheOwnersIs) {
    publish(core::VideoState::Ready);
    for (const std::string_view body :
         {R"({"visibility":"public"})", R"({"visibility":"room:nope"})", "{}", "not json"}) {
        const auto r = service_patch(body);
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 400) << body;
        EXPECT_EQ(r->body, R"({"error":"bad_visibility"})");
    }
    for (const std::string_view video : {kMissing, std::string_view("not-a-uuid")}) {
        const auto r = service_patch(R"({"visibility":"unlisted"})", video);
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 404) << video;
        EXPECT_EQ(r->body, R"({"error":"not_found"})");
    }
    EXPECT_EQ(client_.request("PATCH", service_path(), kService)->status, 400);
}

TEST_F(GatewayVodControl, TheServiceTakesAVideoDown) {
    publish(core::VideoState::Ready, core::Visibility::unlisted());
    ASSERT_EQ(service("POST", grants_path("carol"))->status, 204);
    const auto down = service("DELETE", service_path());
    ASSERT_TRUE(down);
    EXPECT_EQ(down->status, 204);
    for (const std::string_view token : {kAlice, kBob, std::string_view("user.carol")}) {
        expect_gone_for(token);
    }
    EXPECT_EQ(service("GET", grants_path())->status, 404);
    // The owner's own delete now finds nothing to do, as a repeat of the takedown does.
    EXPECT_EQ(remove(kAlice)->status, 204);
    EXPECT_EQ(service("DELETE", service_path())->status, 204);
    EXPECT_EQ(gw_.purges().size(), 1U);
}

TEST_F(GatewayVodControl, TheServiceIsToldWhatItCannotTakeDown) {
    for (const std::string_view video : {kMissing, std::string_view("not-a-uuid")}) {
        const auto r = service("DELETE", service_path(video));
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 404) << video;
        EXPECT_EQ(r->body, R"({"error":"not_found"})");
    }
    publish(core::VideoState::Uploading);
    const auto busy = service("DELETE", service_path());
    ASSERT_TRUE(busy);
    EXPECT_EQ(busy->status, 409);
    EXPECT_EQ(busy->body, R"({"error":"not_committed"})");
    EXPECT_TRUE(gw_.purges().empty());
}

TEST_F(GatewayVodControl, TheServiceListsAUsersVideosNewestFirstAPageAtATime) {
    for (int n = 1; n <= 3; ++n) {
        gw_.put_video(video(n == 2 ? core::VideoState::Processing : core::VideoState::Ready,
                            n == 3 ? core::Visibility::unlisted() : core::Visibility{},
                            nth_video(n)));
    }
    gw_.put_video(video(core::VideoState::Ready, {}, nth_video(4), "bob"));

    const auto all = service("GET", "/api/v1/service/videos?owner=alice");
    ASSERT_TRUE(all);
    ASSERT_EQ(all->status, 200) << all->body;
    EXPECT_EQ(all->header("content-type"), "application/json");
    EXPECT_EQ(all->header("cache-control"), "no-store");
    EXPECT_TRUE(all->body.starts_with(R"({"owner":"alice","videos":[{"id":")" + nth_video(3) +
                                      R"(","title":"trip","state":"ready",)"))
        << all->body;
    const auto third = all->body.find(nth_video(3));
    const auto second = all->body.find(nth_video(2));
    const auto first = all->body.find(nth_video(1));
    EXPECT_LT(third, second);
    EXPECT_LT(second, first);
    EXPECT_NE(first, std::string::npos);
    EXPECT_EQ(all->body.find(nth_video(4)), std::string::npos) << "bob's";
    EXPECT_NE(all->body.find(R"("state":"processing")"), std::string::npos);
    EXPECT_NE(all->body.find(R"("visibility":"unlisted")"), std::string::npos);
    EXPECT_NE(all->body.find(R"(,"created_at":)"), std::string::npos);
    EXPECT_TRUE(all->body.ends_with(R"(],"next":null})")) << all->body;

    const auto page = service("GET", "/api/v1/service/videos?owner=alice&limit=2");
    ASSERT_TRUE(page);
    ASSERT_EQ(page->status, 200);
    EXPECT_EQ(page->body.find(nth_video(1)), std::string::npos) << page->body;
    const auto next = page->body.find(R"("next":")");
    ASSERT_NE(next, std::string::npos) << page->body;
    const std::string cursor =
        page->body.substr(next + 8, page->body.find('"', next + 8) - (next + 8));
    const auto rest = service("GET", "/api/v1/service/videos?limit=2&owner=alice&after=" + cursor);
    ASSERT_TRUE(rest);
    ASSERT_EQ(rest->status, 200) << rest->body;
    EXPECT_NE(rest->body.find(nth_video(1)), std::string::npos) << rest->body;
    EXPECT_EQ(rest->body.find(nth_video(2)), std::string::npos) << rest->body;
    EXPECT_TRUE(rest->body.ends_with(R"(],"next":null})")) << rest->body;
}

TEST_F(GatewayVodControl, ADeletedVideoLeavesTheListing) {
    publish(core::VideoState::Ready);
    ASSERT_NE(service("GET", "/api/v1/service/videos?owner=alice")->body.find(kVideo),
              std::string::npos);
    ASSERT_EQ(remove(kAlice)->status, 204);
    const auto after = service("GET", "/api/v1/service/videos?owner=alice");
    ASSERT_TRUE(after);
    EXPECT_EQ(after->status, 200);
    EXPECT_EQ(after->body, R"({"owner":"alice","videos":[],"next":null})");
}

TEST_F(GatewayVodControl, AUserWithoutVideosHasAnEmptyListing) {
    const auto r = service("GET", "/api/v1/service/videos?owner=auth0%7Cnobody");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    EXPECT_EQ(r->body, R"({"owner":"auth0|nobody","videos":[],"next":null})");
}

TEST_F(GatewayVodControl, AListingsQueryMustNameAnOwnerAndAPageItCanServe) {
    for (const std::string_view query :
         {"", "?limit=10", "?owner=", "?owner=a%20b", "?owner=alice&owner=bob",
          "?owner=alice&limit=0", "?owner=alice&limit=201", "?owner=alice&limit=x",
          "?owner=alice&after=nope", "?owner=alice&after=12."}) {
        const auto r = service("GET", "/api/v1/service/videos" + std::string(query));
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 400) << query;
        EXPECT_EQ(r->body, R"({"error":"bad_query"})");
    }
    EXPECT_EQ(service("GET", "/api/v1/service/videos?owner=alice&limit=200")->status, 200);
}

TEST_F(GatewayVodControl, OnlyTheServicesBearerTokenMayUseTheNewServiceRoutes) {
    publish(core::VideoState::Ready);
    for (const std::string_view token : {kAlice, kBob}) {
        for (const auto& [method, path] :
             std::initializer_list<std::pair<std::string_view, std::string>>{
                 {"GET", "/api/v1/service/videos?owner=alice"},
                 {"GET", "/api/v1/service/videos?limit=0"},
                 {"DELETE", service_path()},
                 {"DELETE", service_path(kMissing)}}) {
            const auto r = service(method, path, token);
            ASSERT_TRUE(r);
            EXPECT_EQ(r->status, 403) << token << " " << method << " " << path;
            EXPECT_EQ(r->body, R"({"error":"forbidden"})");
        }
        const auto patched = service_patch(R"({"visibility":"unlisted"})", kVideo, token);
        ASSERT_TRUE(patched);
        EXPECT_EQ(patched->status, 403) << token;
        EXPECT_EQ(patched->body, R"({"error":"forbidden"})");
    }
    // In the cookie, the service's token is not the service.
    const std::map<std::string, std::string> cookie{
        {"Cookie", "auth_token=service.backend"},
        {"Origin", std::string(ulw::test::kAllowedOrigin)}};
    const auto r = client_.request("DELETE", service_path(), "", {}, cookie);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 403);
    EXPECT_EQ(r->body, R"({"error":"forbidden"})");
    expect_playable_by(kAlice);
    EXPECT_EQ(get(video_path(), kBob)->status, 404);
    EXPECT_EQ(service("GET", "/api/v1/service/videos?owner=alice", "")->status, 401);
    EXPECT_TRUE(gw_.purges().empty());
}

TEST_F(GatewayVodControl, ACatalogOutageIsARetryOnEveryNewRoute) {
    publish(core::VideoState::Ready);
    gw_.fail_catalog(core::ports::CatalogError::Unavailable);
    EXPECT_EQ(remove(kAlice)->status, 503);
    EXPECT_EQ(service("DELETE", service_path())->status, 503);
    EXPECT_EQ(service_patch(R"({"visibility":"unlisted"})")->status, 503);
    EXPECT_EQ(service("GET", "/api/v1/service/videos?owner=alice")->status, 503);
    gw_.fail_catalog(core::ports::CatalogError::Corrupt);
    EXPECT_EQ(service("GET", "/api/v1/service/videos?owner=alice")->status, 500);
    gw_.fail_catalog(std::nullopt);
    expect_playable_by(kAlice);
}

} // namespace
