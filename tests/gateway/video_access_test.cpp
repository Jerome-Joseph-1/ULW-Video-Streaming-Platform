#include "core/models/ids.hpp"
#include "core/models/video.hpp"
#include "core/models/video_access.hpp"
#include "core/models/visibility.hpp"
#include "core/ports/catalog.hpp"

#include "video_access.hpp"

#include <chrono>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <string_view>

namespace {

using gateway::grant_query;
using gateway::user_from_segment;
using gateway::visibility_from_body;

constexpr std::string_view kRoom = "0192f3c4-7a1b-7c2d-8e3f-0123456789ab";
constexpr std::string_view kVideo = "01890a5d-ac96-774b-bcce-b302099a8057";

core::VideoRecord record(core::VideoState state) {
    return core::VideoRecord{
        .id = *core::VideoId::parse(kVideo),
        .owner = *core::UserId::parse("alice"),
        .title = "trip \"one\"",
        .state = state,
        .version = 4,
        .error_reason = state == core::VideoState::Failed ? std::optional<std::string>("too long")
                                                          : std::nullopt,
        .duration =
            state == core::VideoState::Ready ? std::optional(core::Millis{6'000}) : std::nullopt,
        .visibility = core::Visibility::room(*core::RoomId::parse(kRoom))};
}

TEST(UserFromSegment, TakesAUserIdAsIsOrPercentEncoded) {
    EXPECT_EQ(user_from_segment("alice")->view(), "alice");
    EXPECT_EQ(user_from_segment("auth0|123")->view(), "auth0|123");
    EXPECT_EQ(user_from_segment("auth0%7C123")->view(), "auth0|123");
    EXPECT_EQ(user_from_segment("auth0%7c123")->view(), "auth0|123");
    EXPECT_EQ(user_from_segment("a%40b.example")->view(), "a@b.example");
    // '+' is itself in a path, not a space.
    EXPECT_EQ(user_from_segment("a+b")->view(), "a+b");
    EXPECT_EQ(user_from_segment(std::string(128, 'u'))->view(), std::string(128, 'u'));
}

TEST(UserFromSegment, RefusesWhatDoesNotDecodeToAUserId) {
    for (const std::string_view bad :
         {"", "%", "%7", "%7G", "%G7", "a%20b", "a/b", "a%2Fb", "%00", "caf%C3%A9", "x%"}) {
        EXPECT_FALSE(user_from_segment(bad)) << bad;
    }
    EXPECT_FALSE(user_from_segment(std::string(129, 'u')));
    // 128 decoded characters, from more than 128 encoded ones, still fit.
    std::string encoded;
    for (int i = 0; i < 64; ++i) {
        encoded += "%7Ca";
    }
    ASSERT_TRUE(user_from_segment(encoded));
    EXPECT_FALSE(user_from_segment(encoded + "a"));
}

TEST(GrantQuery, DefaultsWithoutAQuery) {
    for (const std::string_view target : {"/x", "/x?", "/x?#frag", "/x?other=1&&"}) {
        const auto q = grant_query(target);
        ASSERT_TRUE(q) << target;
        EXPECT_FALSE(q->after);
        EXPECT_EQ(q->limit, gateway::kDefaultGrantPage);
    }
}

TEST(GrantQuery, ReadsTheCursorAndTheLimit) {
    const auto q = grant_query("/x?limit=2&after=auth0%7Cbob&x=y#f");
    ASSERT_TRUE(q);
    ASSERT_TRUE(q->after);
    EXPECT_EQ(q->after->view(), "auth0|bob");
    EXPECT_EQ(q->limit, 2U);
    EXPECT_EQ(grant_query("/x?limit=1000")->limit, gateway::kMaxGrantPage);
}

TEST(GrantQuery, RefusesAMalformedOrRepeatedParameter) {
    for (const std::string_view target :
         {"/x?limit=0", "/x?limit=1001", "/x?limit=-1", "/x?limit=", "/x?limit", "/x?limit=1x",
          "/x?after=", "/x?after", "/x?after=a%2", "/x?after=a%20b", "/x?limit=1&limit=2",
          "/x?after=a&after=b"}) {
        EXPECT_FALSE(grant_query(target)) << target;
    }
}

TEST(VisibilityFromBody, TakesTheThreeForms) {
    EXPECT_EQ(visibility_from_body(R"({"visibility":"private"})"), core::Visibility{});
    EXPECT_EQ(visibility_from_body(R"({"visibility":"unlisted","other":1})"),
              core::Visibility::unlisted());
    EXPECT_EQ(visibility_from_body(R"({"visibility":"room:)" + std::string(kRoom) + "\"}"),
              core::Visibility::room(*core::RoomId::parse(kRoom)));
}

TEST(VisibilityFromBody, RefusesAnythingElse) {
    for (const std::string_view body :
         {"", "null", "[]", R"("private")", "{}", R"({"visibility":null})", R"({"visibility":1})",
          R"({"visibility":"public"})", R"({"visibility":"room:x"})",
          R"({"visibility":"private")"}) {
        EXPECT_FALSE(visibility_from_body(body)) << body;
    }
}

TEST(VideoJson, TheOwnerSeesTheVisibilityAndWhyItFailed) {
    EXPECT_EQ(gateway::video_json(record(core::VideoState::Failed), core::VideoAccess::Owner),
              R"({"id":")" + std::string(kVideo) +
                  R"(","title":"trip \"one\"","state":"failed","version":4,"duration_ms":null,)"
                  R"("error_reason":"too long","visibility":"room:)" +
                  std::string(kRoom) + R"("})");
}

TEST(VideoJson, AViewerSeesNeither) {
    EXPECT_EQ(gateway::video_json(record(core::VideoState::Failed), core::VideoAccess::Viewer),
              R"({"id":")" + std::string(kVideo) +
                  R"(","title":"trip \"one\"","state":"failed","version":4,"duration_ms":null})");
    EXPECT_EQ(gateway::video_json(record(core::VideoState::Ready), core::VideoAccess::Viewer),
              R"({"id":")" + std::string(kVideo) +
                  R"(","title":"trip \"one\"","state":"ready","version":4,"duration_ms":6000})");
}

TEST(StateName, NamesEveryState) {
    EXPECT_EQ(gateway::state_name(core::VideoState::Init), "init");
    EXPECT_EQ(gateway::state_name(core::VideoState::Uploading), "uploading");
    EXPECT_EQ(gateway::state_name(core::VideoState::Processing), "processing");
    EXPECT_EQ(gateway::state_name(core::VideoState::Ready), "ready");
    EXPECT_EQ(gateway::state_name(core::VideoState::Failed), "failed");
}

TEST(GrantsJson, ListsEachGrantAndTheCursorWhenMoreFollow) {
    const auto video = *core::VideoId::parse(kVideo);
    core::ports::GrantPage page;
    page.grants.push_back({.user = *core::UserId::parse("auth0|bob"),
                           .granted_at = core::WallTime{std::chrono::seconds{1'700'000'000}}});
    page.grants.push_back({.user = *core::UserId::parse("carol"),
                           .granted_at = core::WallTime{std::chrono::milliseconds{1'500}}});
    const std::string head = R"({"video_id":")" + std::string(kVideo) + R"(","grants":[)";
    const std::string grants = R"({"user_id":"auth0|bob","granted_at":1700000000},)"
                               R"({"user_id":"carol","granted_at":1}])";
    EXPECT_EQ(gateway::grants_json(video, page), head + grants + R"(,"next":null})");
    page.more = true;
    EXPECT_EQ(gateway::grants_json(video, page), head + grants + R"(,"next":"carol"})");
    EXPECT_EQ(gateway::grants_json(video, {}), head + R"(],"next":null})");
}

TEST(ErrorJson, CarriesTheCode) {
    EXPECT_EQ(gateway::error_json("not_found"), R"({"error":"not_found"})");
}

// GET /api/v1/service/videos (ADR-0100).
TEST(VideoQuery, NeedsAnOwnerAndDefaultsTheRest) {
    EXPECT_FALSE(gateway::video_query("/api/v1/service/videos"));
    EXPECT_FALSE(gateway::video_query("/api/v1/service/videos?limit=5"));
    const auto q = gateway::video_query("/api/v1/service/videos?owner=auth0%7C123");
    ASSERT_TRUE(q);
    EXPECT_EQ(q->owner.view(), "auth0|123");
    EXPECT_FALSE(q->after);
    EXPECT_EQ(q->limit, gateway::kDefaultVideoPage);
}

TEST(VideoQuery, ReadsTheCursorAndTheLimitAndIgnoresTheRest) {
    const auto q = gateway::video_query("/x?limit=200&after=1759600000123456." +
                                        std::string(kVideo) + "&owner=alice&other=1#frag");
    ASSERT_TRUE(q);
    EXPECT_EQ(q->owner.view(), "alice");
    EXPECT_EQ(q->limit, 200U);
    ASSERT_TRUE(q->after);
    EXPECT_EQ(q->after->created_at_us, 1759600000123456);
    EXPECT_EQ(q->after->id.to_string(), kVideo);
}

TEST(VideoQuery, RefusesAMalformedOrRepeatedParameter) {
    for (const std::string_view target :
         {"/x?owner=", "/x?owner=a%2", "/x?owner=a&owner=b", "/x?owner=a&limit=0",
          "/x?owner=a&limit=201", "/x?owner=a&limit=-1", "/x?owner=a&limit=1&limit=2",
          "/x?owner=a&after=", "/x?owner=a&after=123", "/x?owner=a&after=-1.x",
          "/x?owner=a&after=12.not-a-uuid"}) {
        EXPECT_FALSE(gateway::video_query(target)) << target;
    }
}

TEST(VideoCursor, RoundTripsThroughItsText) {
    const core::ports::VideoCursor cursor{.created_at_us = 42, .id = *core::VideoId::parse(kVideo)};
    const std::string text = gateway::cursor_text(cursor);
    EXPECT_EQ(text, "42." + std::string(kVideo));
    const auto back = gateway::cursor_from_text(text);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->created_at_us, 42);
    EXPECT_EQ(back->id, cursor.id);
    EXPECT_FALSE(gateway::cursor_from_text("-42." + std::string(kVideo)));
}

TEST(VideosJson, ListsEachVideoAsItsOwnerSeesItAndTheCursorWhenMoreFollow) {
    core::ports::VideoPage page;
    page.videos.push_back(
        {.video = record(core::VideoState::Failed), .created_at_us = 1'759'600'000'999'999});
    page.more = true;
    EXPECT_EQ(gateway::videos_json(*core::UserId::parse("alice"), page),
              R"({"owner":"alice","videos":[{"id":")" + std::string(kVideo) +
                  R"(","title":"trip \"one\"","state":"failed","version":4,"duration_ms":null,)"
                  R"("error_reason":"too long","visibility":"room:)" +
                  std::string(kRoom) + R"(","created_at":1759600000}],"next":"1759600000999999.)" +
                  std::string(kVideo) + R"("})");
    page.more = false;
    EXPECT_TRUE(
        gateway::videos_json(*core::UserId::parse("alice"), page).ends_with(R"(],"next":null})"));
    EXPECT_EQ(gateway::videos_json(*core::UserId::parse("alice"), {}),
              R"({"owner":"alice","videos":[],"next":null})");
}

} // namespace
