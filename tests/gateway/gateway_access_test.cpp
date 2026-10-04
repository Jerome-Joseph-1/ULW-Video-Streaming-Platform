#include "gateway_harness.hpp"
#include "support/http_client.hpp"

#include <cstddef>
#include <gtest/gtest.h>
#include <initializer_list>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// Who besides its owner may see a video (ADR-0097), over HTTP: every path that serves one, each
// visibility, the owner's PATCH and the operator's backend's grants.
namespace {

using ulw::test::Backend;
using ulw::test::GatewayOptions;
using ulw::test::GatewayUnderTest;
using ulw::test::HttpClient;
using ulw::test::HttpResponse;

constexpr std::string_view kAlice = "user.alice";
constexpr std::string_view kBob = "user.bob";
constexpr std::string_view kCarol = "user.carol";
constexpr std::string_view kService = "service.backend";
constexpr std::string_view kVideo = "01890a5d-ac96-774b-bcce-b302099a8057";
constexpr std::string_view kMissing = "01890a5d-ac96-774b-bcce-b302099a8058";
constexpr std::string_view kRoom = "0192f3c4-7a1b-7c2d-8e3f-0123456789ab";
constexpr std::string_view kOtherRoom = "0192f3c4-7a1b-7c2d-8e3f-0123456789ac";

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

std::string grants_path(std::string_view user = {}, std::string_view video = kVideo) {
    std::string out = "/api/v1/service/videos/" + std::string(video) + "/grants";
    if (!user.empty()) {
        out += "/" + std::string(user);
    }
    return out;
}

std::string key(std::string_view rest) {
    return "videos/" + std::string(kVideo) + "/hls/" + std::string(rest);
}

std::string room_visibility(std::string_view room = kRoom) {
    return "room:" + std::string(room);
}

core::VideoRecord video(core::VideoState state, core::Visibility visibility,
                        std::string_view owner = "alice") {
    const bool ready = state == core::VideoState::Ready;
    return core::VideoRecord{.id = *core::VideoId::parse(kVideo),
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

core::Visibility in_room(std::string_view room = kRoom) {
    return core::Visibility::room(*core::RoomId::parse(room));
}

class GatewayAccess : public ::testing::Test {
protected:
    [[nodiscard]] static GatewayOptions options() {
        GatewayOptions o;
        o.backend = Backend::Fake;
        return o;
    }

    void publish(core::VideoState state, core::Visibility visibility,
                 std::string_view owner = "alice") {
        gw_.put_video(video(state, visibility, owner));
        gw_.put_object(key("master.m3u8"), kMaster);
        gw_.put_object(key("720p/index.m3u8"), kMedia);
    }

    std::optional<HttpResponse> get(std::string_view path, std::string_view token) {
        return client_.request("GET", path, token);
    }

    std::optional<HttpResponse> patch(std::string_view body, std::string_view token,
                                      std::string_view video = kVideo) {
        return client_.request("PATCH", video_path({}, video), token,
                               std::as_bytes(std::span(body)),
                               {{"Content-Type", "application/json"}});
    }

    std::optional<HttpResponse> service(std::string_view method, std::string_view path,
                                        std::string_view token = kService) {
        return client_.request(method, path, token);
    }

    // What every path that serves the video answers `token`: metadata and both playlists.
    void expect_hidden_from(std::string_view token) {
        for (const std::string& path :
             {video_path(), video_path("master.m3u8"), video_path("720p/index.m3u8")}) {
            const auto r = get(path, token);
            ASSERT_TRUE(r) << path;
            EXPECT_EQ(r->status, 404) << token << " " << path;
            // No signed URL, nor anything else about it.
            EXPECT_TRUE(r->body.empty()) << r->body;
            EXPECT_EQ(r->body.find("seg_"), std::string::npos);
        }
    }

    void expect_playable_by(std::string_view token) {
        const auto meta = get(video_path(), token);
        ASSERT_TRUE(meta);
        EXPECT_EQ(meta->status, 200) << token << " " << meta->body;
        const auto master = get(video_path("master.m3u8"), token);
        ASSERT_TRUE(master);
        EXPECT_EQ(master->status, 200) << token;
        const auto media = get(video_path("720p/index.m3u8"), token);
        ASSERT_TRUE(media);
        EXPECT_EQ(media->status, 200) << token;
        EXPECT_NE(media->body.find("fake://" + key("720p/seg_00000.m4s")), std::string::npos)
            << media->body;
    }

    GatewayUnderTest gw_{options()};
    HttpClient client_{gw_.endpoint()};
};

TEST_F(GatewayAccess, APrivateVideoIsHiddenFromEveryoneButItsOwner) {
    publish(core::VideoState::Ready, {});
    expect_playable_by(kAlice);
    expect_hidden_from(kBob);
    // Hidden exactly as a video that does not exist is.
    const auto missing = get(video_path({}, kMissing), kBob);
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 404);
    EXPECT_EQ(missing->body, "");
}

TEST_F(GatewayAccess, AnUnlistedVideoPlaysForAnyoneSignedIn) {
    publish(core::VideoState::Ready, core::Visibility::unlisted());
    expect_playable_by(kBob);
    expect_playable_by(kCarol);
    EXPECT_EQ(get(video_path("master.m3u8"), "")->status, 401);
}

TEST_F(GatewayAccess, ARoomsVideoPlaysForItsMembersAndNobodyElse) {
    publish(core::VideoState::Ready, in_room());
    gw_.add_member(kRoom, "alice");
    gw_.add_member(kRoom, "bob");
    gw_.add_member(kOtherRoom, "carol");
    expect_playable_by(kBob);
    // Carol is listed in another room only: no playlist, and so no segment URL.
    expect_hidden_from(kCarol);
}

TEST_F(GatewayAccess, TakingAMemberOffTheRoomsListTakesTheVideoAtTheirNextRequest) {
    publish(core::VideoState::Ready, in_room());
    gw_.add_member(kRoom, "alice");
    gw_.add_member(kRoom, "bob");
    expect_playable_by(kBob);
    gw_.remove_member(kRoom, "bob");
    expect_hidden_from(kBob);
    // And listing them again gives it back: nothing is cached either way.
    gw_.add_member(kRoom, "bob");
    expect_playable_by(kBob);
}

TEST_F(GatewayAccess, TheOwnerLeavingTheRoomStopsSharingIntoIt) {
    publish(core::VideoState::Ready, in_room());
    gw_.add_member(kRoom, "alice");
    gw_.add_member(kRoom, "bob");
    expect_playable_by(kBob);
    gw_.remove_member(kRoom, "alice");
    expect_hidden_from(kBob);
    expect_playable_by(kAlice);
}

TEST_F(GatewayAccess, AViewerSeesNeitherTheVisibilityNorWhyTheVideoFailed) {
    publish(core::VideoState::Failed, core::Visibility::unlisted());
    const auto viewer = get(video_path(), kBob);
    ASSERT_TRUE(viewer);
    ASSERT_EQ(viewer->status, 200);
    EXPECT_EQ(viewer->body, R"({"id":")" + std::string(kVideo) +
                                R"(","title":"trip","state":"failed","version":3,)"
                                R"("duration_ms":null})");
    const auto owner = get(video_path(), kAlice);
    ASSERT_TRUE(owner);
    EXPECT_NE(owner->body.find(R"("error_reason":"bad","visibility":"unlisted")"),
              std::string::npos)
        << owner->body;
    // A viewer who may see it learns it will never play, as its owner does.
    EXPECT_EQ(get(video_path("master.m3u8"), kBob)->status, 409);
}

TEST_F(GatewayAccess, AnUploadInProgressStaysTheUploadersWhateverIsSharedOrGranted) {
    for (const core::VideoState state : {core::VideoState::Init, core::VideoState::Uploading}) {
        publish(state, in_room());
        gw_.add_member(kRoom, "alice");
        gw_.add_member(kRoom, "bob");
        ASSERT_EQ(service("POST", grants_path("bob"))->status, 204);
        expect_hidden_from(kBob);
        EXPECT_EQ(get(video_path(), kAlice)->status, 200);
        EXPECT_EQ(get(video_path("master.m3u8"), kAlice)->status, 409);
        // Once committed, both the room and the grant open it.
        publish(core::VideoState::Processing, in_room());
        EXPECT_EQ(get(video_path(), kBob)->status, 200);
        EXPECT_EQ(get(video_path("master.m3u8"), kBob)->status, 409);
    }
}

TEST_F(GatewayAccess, ALiveRecordingIsItsBroadcastersVideoUnderTheSameRules) {
    // A stream's recording becomes a video owned by its broadcaster (live.md), and is shared
    // as any other: private until they say otherwise.
    publish(core::VideoState::Ready, {}, "broadcaster");
    expect_hidden_from(kBob);
    EXPECT_EQ(patch(R"({"visibility":"unlisted"})", "user.broadcaster")->status, 200);
    expect_playable_by(kBob);
}

TEST_F(GatewayAccess, TheOwnerSetsTheVisibility) {
    publish(core::VideoState::Ready, {});
    const auto unlisted = patch(R"({"visibility":"unlisted"})", kAlice);
    ASSERT_TRUE(unlisted);
    ASSERT_EQ(unlisted->status, 200) << unlisted->body;
    EXPECT_EQ(unlisted->header("content-type"), "application/json");
    EXPECT_NE(unlisted->body.find(R"("visibility":"unlisted")"), std::string::npos);
    expect_playable_by(kBob);

    gw_.add_member(kRoom, "alice");
    const auto shared = patch(R"({"visibility":")" + room_visibility() + "\"}", kAlice);
    ASSERT_TRUE(shared);
    ASSERT_EQ(shared->status, 200) << shared->body;
    EXPECT_NE(shared->body.find(R"("visibility":")" + room_visibility() + "\""), std::string::npos);
    expect_hidden_from(kBob);
    gw_.add_member(kRoom, "bob");
    expect_playable_by(kBob);

    ASSERT_EQ(patch(R"({"visibility":"private"})", kAlice)->status, 200);
    expect_hidden_from(kBob);
}

TEST_F(GatewayAccess, OnlyARoomTheOwnerIsAMemberOfMayBeChosen) {
    publish(core::VideoState::Ready, {});
    const auto r = patch(R"({"visibility":")" + room_visibility(kOtherRoom) + "\"}", kAlice);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 403);
    EXPECT_EQ(r->body, R"({"error":"not_member"})");
    EXPECT_NE(get(video_path(), kAlice)->body.find(R"("visibility":"private")"), std::string::npos);
}

TEST_F(GatewayAccess, AViewerMayNotChangeItAndOthersCannotTellItExists) {
    publish(core::VideoState::Ready, core::Visibility::unlisted());
    const auto viewer = patch(R"({"visibility":"private"})", kBob);
    ASSERT_TRUE(viewer);
    EXPECT_EQ(viewer->status, 403);
    EXPECT_EQ(viewer->body, R"({"error":"forbidden"})");

    publish(core::VideoState::Ready, {});
    const auto hidden = patch(R"({"visibility":"unlisted"})", kBob);
    const auto missing = patch(R"({"visibility":"unlisted"})", kBob, kMissing);
    const auto malformed = patch(R"({"visibility":"unlisted"})", kBob, "not-a-uuid");
    ASSERT_TRUE(hidden && missing && malformed);
    for (const auto* r : {&*hidden, &*missing, &*malformed}) {
        EXPECT_EQ(r->status, 404);
        EXPECT_EQ(r->body, R"({"error":"not_found"})");
    }
    expect_hidden_from(kBob);
}

TEST_F(GatewayAccess, AnUnreadableVisibilityIsABadRequest) {
    publish(core::VideoState::Ready, {});
    for (const std::string_view body :
         {R"({"visibility":"public"})", R"({"visibility":"room:nope"})", "{}", "not json"}) {
        const auto r = patch(body, kAlice);
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 400) << body;
        EXPECT_EQ(r->body, R"({"error":"bad_visibility"})");
    }
    // A PATCH without a body says nothing.
    EXPECT_EQ(client_.request("PATCH", video_path(), kAlice)->status, 400);
}

TEST_F(GatewayAccess, ACookiePatchNeedsAJsonBodyFromAnAllowedPage) {
    publish(core::VideoState::Ready, {});
    const std::string body = R"({"visibility":"unlisted"})";
    const auto bytes = std::as_bytes(std::span(body));
    const std::map<std::string, std::string> form{
        {"Cookie", "auth_token=user.alice"},
        {"Origin", std::string(ulw::test::kAllowedOrigin)},
        {"Content-Type", "text/plain"}};
    EXPECT_EQ(client_.request("PATCH", video_path(), "", bytes, form)->status, 403);
    HttpClient other(gw_.endpoint());
    auto json = form;
    json["Content-Type"] = "application/json";
    EXPECT_EQ(other.request("PATCH", video_path(), "", bytes, json)->status, 200);
}

TEST_F(GatewayAccess, TheServiceGrantsAndRevokesSingleUsers) {
    publish(core::VideoState::Ready, {});
    expect_hidden_from(kBob);
    const auto granted = service("POST", grants_path("bob"));
    ASSERT_TRUE(granted);
    EXPECT_EQ(granted->status, 204);
    expect_playable_by(kBob);
    expect_hidden_from(kCarol);
    // Granting again changes nothing.
    EXPECT_EQ(service("POST", grants_path("bob"))->status, 204);

    const auto revoked = service("DELETE", grants_path("bob"));
    ASSERT_TRUE(revoked);
    EXPECT_EQ(revoked->status, 204);
    expect_hidden_from(kBob);
    EXPECT_EQ(service("DELETE", grants_path("bob"))->status, 204);
}

TEST_F(GatewayAccess, TheServiceListsGrantsAPageAtATime) {
    publish(core::VideoState::Ready, {});
    for (const std::string_view user : {"carol", "auth0%7Cdave", "bob"}) {
        ASSERT_EQ(service("POST", grants_path(user))->status, 204) << user;
    }
    const auto all = service("GET", grants_path());
    ASSERT_TRUE(all);
    ASSERT_EQ(all->status, 200) << all->body;
    EXPECT_EQ(all->header("cache-control"), "no-store");
    // Bytewise: "a" < "b" < "c".
    const auto auth0 = all->body.find(R"({"user_id":"auth0|dave","granted_at":)");
    const auto bob = all->body.find(R"({"user_id":"bob","granted_at":)");
    const auto carol = all->body.find(R"({"user_id":"carol","granted_at":)");
    EXPECT_LT(auth0, bob);
    EXPECT_LT(bob, carol);
    EXPECT_NE(carol, std::string::npos) << all->body;
    EXPECT_NE(all->body.find(R"("next":null)"), std::string::npos);

    const auto first = service("GET", grants_path() + "?limit=2");
    ASSERT_TRUE(first);
    EXPECT_EQ(first->body.find("carol"), std::string::npos) << first->body;
    EXPECT_NE(first->body.find(R"("next":"bob")"), std::string::npos) << first->body;
    const auto rest = service("GET", grants_path() + "?limit=2&after=bob");
    ASSERT_TRUE(rest);
    EXPECT_NE(rest->body.find(R"({"user_id":"carol")"), std::string::npos) << rest->body;
    EXPECT_EQ(rest->body.find("bob"), std::string::npos) << rest->body;
    EXPECT_NE(rest->body.find(R"("next":null)"), std::string::npos);

    const auto bad = service("GET", grants_path() + "?limit=0");
    ASSERT_TRUE(bad);
    EXPECT_EQ(bad->status, 400);
    EXPECT_EQ(bad->body, R"({"error":"bad_query"})");
}

TEST_F(GatewayAccess, TheServiceIsTold404ForAVideoThatDoesNotExist) {
    for (const std::string_view method : {"POST", "DELETE"}) {
        const auto r = service(method, grants_path("bob", kMissing));
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 404) << method;
        EXPECT_EQ(r->body, R"({"error":"not_found"})");
    }
    EXPECT_EQ(service("GET", grants_path({}, kMissing))->status, 404);
    EXPECT_EQ(service("GET", grants_path({}, "not-a-uuid"))->status, 404);
}

TEST_F(GatewayAccess, AUserIdThatIsNotOneIsABadRequest) {
    publish(core::VideoState::Ready, {});
    for (const std::string_view user : {"a%20b", "%ZZ", "a%2Fb"}) {
        const auto r = service("POST", grants_path(user));
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 400) << user;
        EXPECT_EQ(r->body, R"({"error":"bad_user"})");
    }
}

TEST_F(GatewayAccess, OnlyTheServicesTokenMayUseTheServiceApi) {
    publish(core::VideoState::Ready, {});
    // The owner included, and whether or not the video exists.
    for (const std::string_view token : {kAlice, kBob}) {
        for (const auto& [method, path] :
             std::initializer_list<std::pair<std::string_view, std::string>>{
                 {"POST", grants_path("bob")},
                 {"DELETE", grants_path("bob")},
                 {"GET", grants_path()},
                 {"POST", grants_path("bob", kMissing)},
                 {"GET", grants_path() + "?limit=0"}}) {
            const auto r = service(method, path, token);
            ASSERT_TRUE(r);
            EXPECT_EQ(r->status, 403) << token << " " << method << " " << path;
            EXPECT_EQ(r->body, R"({"error":"forbidden"})");
        }
    }
    expect_hidden_from(kBob);
    EXPECT_EQ(service("GET", grants_path(), "")->status, 401);
}

TEST_F(GatewayAccess, AServiceTokenInTheCookieIsNotTheService) {
    publish(core::VideoState::Ready, {});
    const std::map<std::string, std::string> cookie{
        {"Cookie", "auth_token=service.backend"},
        {"Origin", std::string(ulw::test::kAllowedOrigin)}};
    for (const auto& [method, path] :
         std::initializer_list<std::pair<std::string_view, std::string>>{
             {"POST", grants_path("bob")},
             {"DELETE", grants_path("bob")},
             {"GET", grants_path()}}) {
        const auto r = client_.request(method, path, "", {}, cookie);
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 403) << method;
        EXPECT_EQ(r->body, R"({"error":"forbidden"})");
    }
    expect_hidden_from(kBob);
}

TEST_F(GatewayAccess, AServiceRequestCarriesNoBody) {
    publish(core::VideoState::Ready, {});
    const std::string body = "{}";
    EXPECT_EQ(client_.request("POST", grants_path("bob"), kService, std::as_bytes(std::span(body)))
                  ->status,
              400);
}

TEST_F(GatewayAccess, ACatalogOutageIsARetryOnEveryAccessRoute) {
    publish(core::VideoState::Ready, {});
    gw_.fail_catalog(core::ports::CatalogError::Unavailable);
    EXPECT_EQ(patch(R"({"visibility":"unlisted"})", kAlice)->status, 503);
    EXPECT_EQ(service("POST", grants_path("bob"))->status, 503);
    EXPECT_EQ(service("DELETE", grants_path("bob"))->status, 503);
    EXPECT_EQ(service("GET", grants_path())->status, 503);
    EXPECT_EQ(get(video_path(), kAlice)->status, 503);
    gw_.fail_catalog(core::ports::CatalogError::Corrupt);
    EXPECT_EQ(get(video_path("master.m3u8"), kAlice)->status, 500);
    EXPECT_EQ(service("GET", grants_path())->status, 500);
}

} // namespace
