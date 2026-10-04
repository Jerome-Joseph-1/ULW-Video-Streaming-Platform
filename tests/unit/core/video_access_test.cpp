#include "core/errors/domain_error.hpp"
#include "core/models/ids.hpp"
#include "core/models/video.hpp"
#include "core/models/video_access.hpp"
#include "core/models/visibility.hpp"

#include <array>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <string_view>

namespace {

using core::DomainError;
using core::VideoAccess;
using core::VideoState;
using core::ViewerFacts;
using core::Visibility;
using core::VisibilityKind;

constexpr std::string_view kRoom = "0192f3c4-7a1b-7c2d-8e3f-0123456789ab";

core::RoomId room() {
    return *core::RoomId::parse(kRoom);
}

core::UserId user(std::string_view name) {
    return *core::UserId::parse(name);
}

core::VideoRecord video(VideoState state, Visibility visibility) {
    const bool ready = state == VideoState::Ready;
    return core::VideoRecord{.id = *core::VideoId::parse("01890a5d-ac96-774b-bcce-b302099a8057"),
                             .owner = user("alice"),
                             .title = "trip",
                             .state = state,
                             .version = 1,
                             .error_reason = state == VideoState::Failed
                                                 ? std::optional<std::string>("bad")
                                                 : std::nullopt,
                             .duration = ready ? std::optional(core::Millis{5'000}) : std::nullopt,
                             .visibility = visibility};
}

constexpr std::array kEveryState{VideoState::Init, VideoState::Uploading, VideoState::Processing,
                                 VideoState::Ready, VideoState::Failed};

TEST(Visibility, EveryVideoStartsPrivate) {
    const Visibility v;
    EXPECT_EQ(v.kind(), VisibilityKind::Private);
    EXPECT_FALSE(v.room_id());
    EXPECT_EQ(v.to_string(), "private");
}

TEST(Visibility, ParsesTheThreeFormsAndWritesThemBack) {
    for (const std::string_view text :
         {std::string_view{"private"}, std::string_view{"unlisted"},
          std::string_view{"room:0192f3c4-7a1b-7c2d-8e3f-0123456789ab"}}) {
        const auto v = Visibility::parse(text);
        ASSERT_TRUE(v) << text;
        EXPECT_EQ(v->to_string(), text);
    }
    const auto shared = Visibility::parse("room:" + std::string(kRoom));
    ASSERT_TRUE(shared);
    EXPECT_EQ(shared->kind(), VisibilityKind::Room);
    EXPECT_EQ(shared->room_id(), room());
    EXPECT_EQ(shared->kind_name(), "room");
    EXPECT_EQ(Visibility::parse("unlisted")->kind_name(), "unlisted");
    EXPECT_EQ(Visibility::parse("private")->kind_name(), "private");
    EXPECT_EQ(*shared, Visibility::room(room()));
    EXPECT_NE(*shared, Visibility::unlisted());
}

TEST(Visibility, RefusesAnythingElse) {
    for (const std::string_view text :
         {"", "Private", "public", "room", "room:", "room:not-a-uuid",
          // Ids are lowercase canonical UUIDs, as everywhere in the API.
          "room:0192F3C4-7A1B-7C2D-8E3F-0123456789AB", " private", "unlisted ",
          "room: 0192f3c4-7a1b-7c2d-8e3f-0123456789ab"}) {
        EXPECT_EQ(Visibility::parse(text).error_or(DomainError::CorruptRecord),
                  DomainError::InvalidVisibility)
            << '"' << text << '"';
    }
}

TEST(Visibility, ReadsTheCatalogsColumnsOnlyAsTheApiWritesThem) {
    EXPECT_EQ(Visibility::from_columns("private", std::nullopt), Visibility{});
    EXPECT_EQ(Visibility::from_columns("unlisted", std::nullopt), Visibility::unlisted());
    EXPECT_EQ(Visibility::from_columns("room", kRoom), Visibility::room(room()));
    // A room's kind needs its room, and only it may have one.
    EXPECT_FALSE(Visibility::from_columns("room", std::nullopt));
    EXPECT_FALSE(Visibility::from_columns("room", "nope"));
    EXPECT_FALSE(Visibility::from_columns("private", kRoom));
    EXPECT_FALSE(Visibility::from_columns("unlisted", kRoom));
    EXPECT_FALSE(Visibility::from_columns("public", std::nullopt));
}

TEST(VideoAccess, TheOwnerSeesEveryStateUnderEveryVisibility) {
    for (const VideoState state : kEveryState) {
        for (const Visibility& v :
             {Visibility{}, Visibility::unlisted(), Visibility::room(room())}) {
            EXPECT_EQ(core::access_of(video(state, v), user("alice"), {}), VideoAccess::Owner);
        }
    }
}

TEST(VideoAccess, APrivateVideoIsNobodyElsesWithoutAGrant) {
    for (const VideoState state : kEveryState) {
        // Being listed in some room says nothing about a video that names none.
        EXPECT_EQ(core::access_of(video(state, {}), user("bob"), {.room_member = true}),
                  VideoAccess::None);
    }
}

TEST(VideoAccess, AnUnlistedVideoIsAnyonesOnceItsUploadIsCommitted) {
    for (const VideoState state : {VideoState::Processing, VideoState::Ready, VideoState::Failed}) {
        EXPECT_EQ(core::access_of(video(state, Visibility::unlisted()), user("bob"), {}),
                  VideoAccess::Viewer);
    }
}

TEST(VideoAccess, ARoomsVideoIsItsCurrentMembersOnly) {
    const auto shared = video(VideoState::Ready, Visibility::room(room()));
    EXPECT_EQ(core::access_of(shared, user("bob"), {.room_member = true}), VideoAccess::Viewer);
    EXPECT_EQ(core::access_of(shared, user("carol"), {.room_member = false}), VideoAccess::None);
}

TEST(VideoAccess, AGrantOpensAVideoWhateverItsVisibility) {
    for (const Visibility& v : {Visibility{}, Visibility::unlisted(), Visibility::room(room())}) {
        EXPECT_EQ(core::access_of(video(VideoState::Ready, v), user("dave"), {.granted = true}),
                  VideoAccess::Viewer);
    }
}

TEST(VideoAccess, AnUploadInProgressStaysTheUploadersWhateverElseHolds) {
    for (const VideoState state : {VideoState::Init, VideoState::Uploading}) {
        for (const Visibility& v :
             {Visibility{}, Visibility::unlisted(), Visibility::room(room())}) {
            EXPECT_EQ(core::access_of(video(state, v), user("bob"),
                                      ViewerFacts{.room_member = true, .granted = true}),
                      VideoAccess::None);
        }
    }
}

} // namespace
