#include "core/util/hls.hpp"

#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <vector>

namespace {

using core::hls::PlaylistError;
using core::hls::rewrite_master;
using core::hls::rewrite_media;

constexpr std::string_view kDir = "videos/v1/hls/720p/";
constexpr std::string_view kRoute = "/api/v1/videos/v1/";

// As ffmpeg 6.1 writes them, blank lines included.
constexpr std::string_view kMaster = "#EXTM3U\n"
                                     "#EXT-X-VERSION:7\n"
                                     "#EXT-X-STREAM-INF:BANDWIDTH=1020800,RESOLUTION=1280x720,"
                                     "CODECS=\"avc1.4d401f,mp4a.40.2\"\n"
                                     "720p/index.m3u8\n"
                                     "\n"
                                     "#EXT-X-STREAM-INF:BANDWIDTH=580800,RESOLUTION=640x360,"
                                     "CODECS=\"avc1.4d401e,mp4a.40.2\"\n"
                                     "360p/index.m3u8\n"
                                     "\n";

constexpr std::string_view kMedia = "#EXTM3U\n"
                                    "#EXT-X-VERSION:7\n"
                                    "#EXT-X-TARGETDURATION:4\n"
                                    "#EXT-X-MEDIA-SEQUENCE:0\n"
                                    "#EXT-X-PLAYLIST-TYPE:VOD\n"
                                    "#EXT-X-INDEPENDENT-SEGMENTS\n"
                                    "#EXT-X-MAP:URI=\"init_0.mp4\"\n"
                                    "#EXTINF:4.000000,\n"
                                    "seg_00000.m4s\n"
                                    "#EXTINF:2.500000,\n"
                                    "seg_00001.m4s\n"
                                    "#EXT-X-ENDLIST\n";

// Signs every key as a URL that names it, and records what it was asked for.
struct RecordingSigner {
    std::vector<std::string> keys;

    core::hls::Signer signer() {
        return [this](const core::StorageKey& key) -> std::optional<std::string> {
            keys.emplace_back(key.view());
            return "https://store.example/" + key.str() + "?sig=1";
        };
    }
};

std::expected<std::string, PlaylistError> media_with(std::string_view line) {
    RecordingSigner s;
    const std::string text = "#EXTM3U\n#EXTINF:4.0,\n" + std::string(line) + "\n";
    return rewrite_media(text, kDir, s.signer());
}

TEST(HlsMaster, RoutesEachVariantThroughTheGatewayAndKeepsEveryTag) {
    const auto out = rewrite_master(kMaster, kRoute);
    ASSERT_TRUE(out) << core::hls::to_string(out.error());
    EXPECT_EQ(*out, "#EXTM3U\n"
                    "#EXT-X-VERSION:7\n"
                    "#EXT-X-STREAM-INF:BANDWIDTH=1020800,RESOLUTION=1280x720,"
                    "CODECS=\"avc1.4d401f,mp4a.40.2\"\n"
                    "/api/v1/videos/v1/720p/index.m3u8\n"
                    "#EXT-X-STREAM-INF:BANDWIDTH=580800,RESOLUTION=640x360,"
                    "CODECS=\"avc1.4d401e,mp4a.40.2\"\n"
                    "/api/v1/videos/v1/360p/index.m3u8\n");
}

TEST(HlsMaster, ListsTheRenditionsItNamesInOrder) {
    const auto names = core::hls::list_renditions(kMaster);
    ASSERT_TRUE(names);
    EXPECT_EQ(*names, (std::vector<std::string_view>{"720p", "360p"}));
}

TEST(HlsMaster, RoutesUriAttributesOfAlternateRenditionsToo) {
    const auto out = rewrite_master(
        "#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"en\",URI=\"audio/index.m3u8\"\n",
        kRoute);
    ASSERT_TRUE(out);
    EXPECT_EQ(*out, "#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"en\","
                    "URI=\"/api/v1/videos/v1/audio/index.m3u8\"\n");
}

TEST(HlsMaster, RefusesVariantsNoRouteServes) {
    for (const std::string_view uri :
         {"720p/other.m3u8", "a/b/index.m3u8", "index.m3u8", "720p/index.m3u8.bak"}) {
        const std::string text = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\n" + std::string(uri);
        EXPECT_EQ(rewrite_master(text, kRoute), std::unexpected(PlaylistError::UnroutableVariant))
            << uri;
        EXPECT_EQ(core::hls::list_renditions(text),
                  std::unexpected(PlaylistError::UnroutableVariant))
            << uri;
    }
}

TEST(HlsMaster, RefusesVariantsOutsideItsDirectory) {
    for (const std::string_view uri :
         {"../other/720p/index.m3u8", "/videos/v2/hls/720p/index.m3u8",
          "https://evil.example/720p/index.m3u8", "./720p/index.m3u8", "720p//index.m3u8"}) {
        const std::string text = "#EXTM3U\n" + std::string(uri) + "\n";
        EXPECT_EQ(rewrite_master(text, kRoute), std::unexpected(PlaylistError::UnsafeUri)) << uri;
    }
}

TEST(HlsMedia, SignsEverySegmentAndTheInitSegmentResolvedAgainstItsDirectory) {
    RecordingSigner s;
    const auto out = rewrite_media(kMedia, kDir, s.signer());
    ASSERT_TRUE(out) << core::hls::to_string(out.error());
    EXPECT_EQ(s.keys, (std::vector<std::string>{"videos/v1/hls/720p/init_0.mp4",
                                                "videos/v1/hls/720p/seg_00000.m4s",
                                                "videos/v1/hls/720p/seg_00001.m4s"}));
    EXPECT_EQ(*out, "#EXTM3U\n"
                    "#EXT-X-VERSION:7\n"
                    "#EXT-X-TARGETDURATION:4\n"
                    "#EXT-X-MEDIA-SEQUENCE:0\n"
                    "#EXT-X-PLAYLIST-TYPE:VOD\n"
                    "#EXT-X-INDEPENDENT-SEGMENTS\n"
                    "#EXT-X-MAP:URI=\"https://store.example/videos/v1/hls/720p/init_0.mp4?sig=1\"\n"
                    "#EXTINF:4.000000,\n"
                    "https://store.example/videos/v1/hls/720p/seg_00000.m4s?sig=1\n"
                    "#EXTINF:2.500000,\n"
                    "https://store.example/videos/v1/hls/720p/seg_00001.m4s?sig=1\n"
                    "#EXT-X-ENDLIST\n");
}

TEST(HlsMedia, KeepsOtherAttributesOfAMapTagAroundTheSignedUri) {
    RecordingSigner s;
    const auto out = rewrite_media("#EXTM3U\n#EXT-X-MAP:BYTERANGE=\"720@0\",URI=\"init.mp4\"\n",
                                   kDir, s.signer());
    ASSERT_TRUE(out);
    EXPECT_EQ(*out, "#EXTM3U\n#EXT-X-MAP:BYTERANGE=\"720@0\","
                    "URI=\"https://store.example/videos/v1/hls/720p/init.mp4?sig=1\"\n");
}

TEST(HlsMedia, ResolvesSubdirectoriesBelowThePlaylist) {
    RecordingSigner s;
    ASSERT_TRUE(rewrite_media("#EXTM3U\n#EXTINF:4,\nparts/seg_1.m4s\n", kDir, s.signer()));
    EXPECT_EQ(s.keys, (std::vector<std::string>{"videos/v1/hls/720p/parts/seg_1.m4s"}));
}

TEST(HlsMedia, RefusesUrisThatCouldNameAKeyOutsideTheVideo) {
    for (const std::string_view uri :
         {"../360p/seg_00000.m4s", "a/../../x.m4s", "..", "/videos/v2/hls/720p/seg_00000.m4s",
          "http://127.0.0.1:9000/bucket/x.m4s", "s3:bucket/x.m4s", "seg.m4s?x=1", "seg.m4s#t=1",
          "seg%2F.m4s", "./seg.m4s", "a//seg.m4s", " seg.m4s"}) {
        EXPECT_EQ(media_with(uri), std::unexpected(PlaylistError::UnsafeUri)) << uri;
    }
}

TEST(HlsMedia, RefusesAnUnsafeInitSegmentUri) {
    RecordingSigner s;
    EXPECT_EQ(
        rewrite_media("#EXTM3U\n#EXT-X-MAP:URI=\"../../v2/hls/720p/init.mp4\"\n", kDir, s.signer()),
        std::unexpected(PlaylistError::UnsafeUri));
    EXPECT_TRUE(s.keys.empty());
}

TEST(HlsMedia, NeverLeavesABlankLineAfterExtinf) {
    RecordingSigner s;
    const auto out = rewrite_media(
        "#EXTM3U\r\n#EXTINF:4.0,\r\n\r\nseg_1.m4s\r\n\n#EXT-X-ENDLIST\r\n", kDir, s.signer());
    ASSERT_TRUE(out);
    EXPECT_EQ(*out, "#EXTM3U\n#EXTINF:4.0,\n"
                    "https://store.example/videos/v1/hls/720p/seg_1.m4s?sig=1\n"
                    "#EXT-X-ENDLIST\n");
}

TEST(HlsMedia, KeepsCommentsVerbatim) {
    RecordingSigner s;
    const auto out = rewrite_media("#EXTM3U\n# a comment, URI=\"x\"\n", kDir, s.signer());
    ASSERT_TRUE(out);
    EXPECT_EQ(*out, "#EXTM3U\n# a comment, URI=\"x\"\n");
    EXPECT_TRUE(s.keys.empty());
}

TEST(HlsPlaylist, RefusesTagsThatCouldCarryAUriItDoesNotHandle) {
    constexpr std::string_view kSteering =
        R"(#EXT-X-CONTENT-STEERING:SERVER-URI="https://evil.example/steer",PATHWAY-ID="a")";
    constexpr std::string_view kInterstitial =
        R"(#EXT-X-DATERANGE:ID="ad",CLASS="com.apple.hls.interstitial",)"
        R"(START-DATE="2026-01-01T00:00:00Z",X-ASSET-URI="https://evil.example/ad.m3u8")";
    for (const std::string_view line :
         {kSteering, kInterstitial, std::string_view{R"(#EXT-X-FUTURE:URI="x")"},
          std::string_view{"#EXT-X-FUTURE"},
          std::string_view{R"(#EXT-X-STREAM-INF:BANDWIDTH=1,X-EVIL-URI="https://evil.example/")"},
          std::string_view{R"(#EXT-X-MAP:X-OTHER-URI="https://evil.example/",URI="init.mp4")"},
          std::string_view{R"(#EXT-X-STREAM-INF:BANDWIDTH=1,URI="720p/index.m3u8")"}}) {
        RecordingSigner s;
        const std::string text = "#EXTM3U\n" + std::string(line) + "\n";
        EXPECT_EQ(rewrite_media(text, kDir, s.signer()), std::unexpected(PlaylistError::UnknownTag))
            << line;
        EXPECT_EQ(rewrite_master(text, kRoute), std::unexpected(PlaylistError::UnknownTag)) << line;
    }
}

TEST(HlsPlaylist, PassesEveryTagTheWorkersFfmpegWrites) {
    RecordingSigner s;
    EXPECT_TRUE(rewrite_media(kMedia, kDir, s.signer()));
    EXPECT_TRUE(rewrite_master(kMaster, kRoute));
}

TEST(HlsMedia, SignsAtMostTheBoundNumberOfUris) {
    std::string text = "#EXTM3U\n";
    for (std::size_t i = 0; i < core::hls::kMaxPlaylistUris; ++i) {
        text += "#EXTINF:4,\nseg.m4s\n";
    }
    RecordingSigner s;
    ASSERT_TRUE(rewrite_media(text, kDir, s.signer()));
    EXPECT_EQ(s.keys.size(), core::hls::kMaxPlaylistUris);

    text += "#EXTINF:4,\nseg.m4s\n";
    RecordingSigner t;
    EXPECT_EQ(rewrite_media(text, kDir, t.signer()), std::unexpected(PlaylistError::TooManyUris));
    EXPECT_EQ(t.keys.size(), core::hls::kMaxPlaylistUris);
}

TEST(HlsMedia, FailsWhenTheSignerRefuses) {
    const auto out = rewrite_media(
        kMedia, kDir, [](const core::StorageKey&) { return std::optional<std::string>{}; });
    EXPECT_EQ(out, std::unexpected(PlaylistError::Unsigned));
}

TEST(HlsMedia, RefusesMalformedUriAttributes) {
    for (const std::string_view tag :
         {"#EXT-X-MAP:URI=init.mp4", "#EXT-X-MAP:URI=\"init.mp4", "#EXT-X-MAP:URI",
          "#EXT-X-MAP:URI=\"init.mp4\"junk", "#EXT-X-MAP:URI=\"\""}) {
        RecordingSigner s;
        const std::string text = "#EXTM3U\n" + std::string(tag) + "\n";
        const auto out = rewrite_media(text, kDir, s.signer());
        ASSERT_FALSE(out) << tag;
        EXPECT_NE(out.error(), PlaylistError::Unsigned) << tag;
        EXPECT_TRUE(s.keys.empty()) << tag;
    }
}

TEST(HlsPlaylist, RefusesControlCharactersInsideALine) {
    using namespace std::string_view_literals;
    for (const std::string_view text :
         {"#EXTM3U\n#EXTINF:4,\rseg.m4s\n"sv, "#EXTM3U\n#EXTINF:4,\nseg\0.m4s\n"sv,
          "#EXTM3U\n#EXT-X-VERSION:7\x1b\n"sv}) {
        RecordingSigner s;
        EXPECT_EQ(rewrite_media(text, kDir, s.signer()), std::unexpected(PlaylistError::Malformed));
        EXPECT_EQ(rewrite_master(text, kRoute), std::unexpected(PlaylistError::Malformed));
    }
}

TEST(HlsPlaylist, RefusesTextWithoutTheHeader) {
    RecordingSigner s;
    EXPECT_EQ(rewrite_media("#EXTINF:4,\nseg.m4s\n", kDir, s.signer()),
              std::unexpected(PlaylistError::Malformed));
    EXPECT_EQ(rewrite_master("", kRoute), std::unexpected(PlaylistError::Malformed));
    EXPECT_EQ(core::hls::list_renditions("#EXTM3U8\n"), std::unexpected(PlaylistError::Malformed));
}

TEST(HlsPresignTtl, IsTwiceTheDurationWithAnHourFloor) {
    using std::chrono::hours;
    using std::chrono::minutes;
    EXPECT_EQ(core::hls::presign_ttl(core::Millis{0}), hours(1));
    EXPECT_EQ(core::hls::presign_ttl(minutes(29)), hours(1));
    EXPECT_EQ(core::hls::presign_ttl(minutes(31)), minutes(62));
    EXPECT_EQ(core::hls::presign_ttl(hours(2)), hours(4));
    // Rounded up, so a URL never expires before twice the length has passed.
    EXPECT_EQ(core::hls::presign_ttl(hours(1) + core::Millis{1}), core::Seconds{7201});
}

} // namespace
