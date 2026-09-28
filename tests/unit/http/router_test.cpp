#include "http/method.hpp"
#include "http/router.hpp"
#include "http/status.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <string_view>

namespace {

using http::Method;
using http::MethodSet;
using http::Route;
using http::Router;
using http::Status;

enum class R : std::uint8_t {
    CreateUpload,
    AppendUpload,
    UploadStatus,
    CancelUpload,
    CommitUpload,
    GetVideo,
    MasterPlaylist,
    MediaPlaylist,
    Healthz,
    Readyz,
    Metrics,
};

constexpr std::array<Route<R>, 11> kRoutes{{
    {.method = Method::Post, .pattern = "/api/v1/uploads", .id = R::CreateUpload},
    {.method = Method::Patch, .pattern = "/api/v1/uploads/{id}", .id = R::AppendUpload},
    {.method = Method::Head, .pattern = "/api/v1/uploads/{id}", .id = R::UploadStatus},
    {.method = Method::Delete, .pattern = "/api/v1/uploads/{id}", .id = R::CancelUpload},
    {.method = Method::Post, .pattern = "/api/v1/uploads/{id}/commit", .id = R::CommitUpload},
    {.method = Method::Get, .pattern = "/api/v1/videos/{id}", .id = R::GetVideo},
    {.method = Method::Get, .pattern = "/api/v1/videos/{id}/master.m3u8", .id = R::MasterPlaylist},
    {.method = Method::Get,
     .pattern = "/api/v1/videos/{id}/{rendition}/index.m3u8",
     .id = R::MediaPlaylist},
    {.method = Method::Get, .pattern = "/api/v1/healthz", .id = R::Healthz},
    {.method = Method::Get, .pattern = "/api/v1/readyz", .id = R::Readyz},
    {.method = Method::Get, .pattern = "/metrics", .id = R::Metrics},
}};

constexpr Router<R> kRouter{kRoutes};

struct Expected {
    Method method;
    std::string_view target;
    R id;
    http::PathParams params;
};

TEST(Router, RoutesEveryEndpointAndCapturesItsParams) {
    const std::array cases{
        Expected{.method = Method::Post,
                 .target = "/api/v1/uploads",
                 .id = R::CreateUpload,
                 .params = {}},
        Expected{.method = Method::Patch,
                 .target = "/api/v1/uploads/u-1",
                 .id = R::AppendUpload,
                 .params = {"u-1"}},
        Expected{.method = Method::Head,
                 .target = "/api/v1/uploads/u-2",
                 .id = R::UploadStatus,
                 .params = {"u-2"}},
        Expected{.method = Method::Delete,
                 .target = "/api/v1/uploads/u-3",
                 .id = R::CancelUpload,
                 .params = {"u-3"}},
        Expected{.method = Method::Post,
                 .target = "/api/v1/uploads/u-4/commit",
                 .id = R::CommitUpload,
                 .params = {"u-4"}},
        Expected{.method = Method::Get,
                 .target = "/api/v1/videos/v-1",
                 .id = R::GetVideo,
                 .params = {"v-1"}},
        Expected{.method = Method::Get,
                 .target = "/api/v1/videos/v-2/master.m3u8",
                 .id = R::MasterPlaylist,
                 .params = {"v-2"}},
        Expected{.method = Method::Get,
                 .target = "/api/v1/videos/v-3/720p/index.m3u8",
                 .id = R::MediaPlaylist,
                 .params = {"v-3", "720p"}},
        Expected{
            .method = Method::Get, .target = "/api/v1/healthz", .id = R::Healthz, .params = {}},
        Expected{.method = Method::Get, .target = "/api/v1/readyz", .id = R::Readyz, .params = {}},
        Expected{.method = Method::Get, .target = "/metrics", .id = R::Metrics, .params = {}},
    };

    for (const Expected& c : cases) {
        const auto m = kRouter.match(c.method, c.target);
        ASSERT_TRUE(m.has_value()) << c.target;
        EXPECT_EQ(m->id, c.id) << c.target;
        EXPECT_EQ(m->params, c.params) << c.target;
    }
}

TEST(Router, ParamsAreViewsIntoTheTarget) {
    const std::string_view target = "/api/v1/videos/abc/1080p/index.m3u8";
    const auto m = kRouter.match(Method::Get, target);

    ASSERT_TRUE(m.has_value());
    const std::less_equal<> le;
    const char* const first = target.data();
    const char* const last = std::to_address(target.end());
    for (const std::string_view p : {m->params[0], m->params[1]}) {
        EXPECT_TRUE(le(first, p.data()) && le(std::to_address(p.end()), last)) << p;
    }
}

TEST(Router, UnknownPathIsNotFound) {
    for (const std::string_view target :
         {"/", "/api", "/api/v1", "/api/v1/nope", "/api/v1/uploads/u/x", "/metrics/x"}) {
        const auto m = kRouter.match(Method::Get, target);
        ASSERT_FALSE(m.has_value()) << target;
        EXPECT_EQ(m.error().status, Status::NotFound) << target;
        EXPECT_TRUE(m.error().allow.empty()) << target;
    }
}

TEST(Router, KnownPathWithAnotherMethodListsTheAllowedOnes) {
    struct Case {
        Method method;
        std::string_view target;
        MethodSet allow;
    };
    const std::array cases{
        Case{.method = Method::Put,
             .target = "/api/v1/uploads/u",
             .allow = {Method::Patch, Method::Head, Method::Delete}},
        Case{.method = Method::Get, .target = "/api/v1/uploads", .allow = {Method::Post}},
        Case{.method = Method::Options,
             .target = "/api/v1/uploads/u/commit",
             .allow = {Method::Post}},
        Case{.method = Method::Delete, .target = "/metrics?x=1", .allow = {Method::Get}},
    };

    for (const Case& c : cases) {
        const auto m = kRouter.match(c.method, c.target);
        ASSERT_FALSE(m.has_value()) << c.target;
        EXPECT_EQ(m.error().status, Status::MethodNotAllowed) << c.target;
        EXPECT_TRUE(m.error().allow == c.allow) << c.target;
    }
}

TEST(Router, UnrecognisedMethodIsNotImplementedAnywhere) {
    for (const std::string_view target : {"/metrics", "/nope"}) {
        const auto m = kRouter.match(Method::Other, target);
        ASSERT_FALSE(m.has_value()) << target;
        EXPECT_EQ(m.error().status, Status::NotImplemented) << target;
    }
}

TEST(Router, QueryAndFragmentAreNotPartOfThePath) {
    const auto video = kRouter.match(Method::Get, "/api/v1/videos/abc?sig=a/b&exp=1");
    ASSERT_TRUE(video.has_value());
    EXPECT_EQ(video->id, R::GetVideo);
    EXPECT_EQ(video->params[0], "abc");

    const auto health = kRouter.match(Method::Get, "/api/v1/healthz#top");
    ASSERT_TRUE(health.has_value());
    EXPECT_EQ(health->id, R::Healthz);
    EXPECT_TRUE(kRouter.match(Method::Get, "/metrics?").has_value());
}

TEST(Router, TrailingSlashIsADifferentPath) {
    for (const std::string_view target :
         {"/api/v1/healthz/", "/api/v1/videos/abc/", "/api/v1/uploads/", "/metrics/?a=1"}) {
        const auto m = kRouter.match(Method::Get, target);
        ASSERT_FALSE(m.has_value()) << target;
        EXPECT_EQ(m.error().status, Status::NotFound) << target;
    }
}

TEST(Router, EmptySegmentNeverFillsAParam) {
    for (const std::string_view target :
         {"/api/v1/videos//master.m3u8", "/api/v1/videos/", "/api/v1/videos/v//index.m3u8"}) {
        EXPECT_FALSE(kRouter.match(Method::Get, target).has_value()) << target;
    }
}

TEST(Router, DotSegmentsAndEscapesAreCapturedNotResolved) {
    const auto up = kRouter.match(Method::Get, "/api/v1/videos/../master.m3u8");
    ASSERT_TRUE(up.has_value());
    EXPECT_EQ(up->id, R::MasterPlaylist);
    EXPECT_EQ(up->params[0], "..");

    const auto rendition = kRouter.match(Method::Get, "/api/v1/videos/v/../index.m3u8");
    ASSERT_TRUE(rendition.has_value());
    EXPECT_EQ(rendition->id, R::MediaPlaylist);
    EXPECT_EQ(rendition->params[1], "..");

    const auto escaped = kRouter.match(Method::Get, "/api/v1/videos/%2E%2E%2Fx");
    ASSERT_TRUE(escaped.has_value());
    EXPECT_EQ(escaped->params[0], "%2E%2E%2Fx");
}

TEST(Router, LiteralSegmentsAreCaseSensitive) {
    EXPECT_FALSE(kRouter.match(Method::Get, "/API/v1/healthz").has_value());
    EXPECT_FALSE(kRouter.match(Method::Get, "/Metrics").has_value());
}

TEST(Router, TargetsNotInOriginFormAreNotFound) {
    for (const std::string_view target : {"*", "http://api.example/metrics", "metrics", ""}) {
        const auto m = kRouter.match(Method::Get, target);
        ASSERT_FALSE(m.has_value()) << target;
        EXPECT_EQ(m.error().status, Status::NotFound) << target;
    }
}

static_assert(http::detail::is_valid_pattern("/"));
static_assert(http::detail::is_valid_pattern("/a/{b}/c.m3u8"));
static_assert(http::detail::is_valid_pattern("/{a}/{b}/{c}/{d}"));
static_assert(!http::detail::is_valid_pattern(""));
static_assert(!http::detail::is_valid_pattern("a/b"));
static_assert(!http::detail::is_valid_pattern("/a//b"));
static_assert(!http::detail::is_valid_pattern("/a/"));
static_assert(!http::detail::is_valid_pattern("/{}"));
static_assert(!http::detail::is_valid_pattern("/{a"));
static_assert(!http::detail::is_valid_pattern("/a{b}"));
static_assert(!http::detail::is_valid_pattern("/{a}{b}"));
static_assert(!http::detail::is_valid_pattern("/a?b"));
static_assert(!http::detail::is_valid_pattern("/{a}/{b}/{c}/{d}/{e}"));

} // namespace
