#include "media_playlist.hpp"

#include <chrono>
#include <gtest/gtest.h>
#include <string>

namespace {

using live::MediaPlaylist;
using live::Micros;
using live::PlaylistError;
using live::Segment;

// What ffmpeg 6.1 wrote for a live remux of MPEG-TS, `-hls_segment_type fmp4 -start_number 7`,
// after the publisher went away.
constexpr std::string_view kFfmpegPlaylist = "#EXTM3U\n"
                                             "#EXT-X-VERSION:7\n"
                                             "#EXT-X-TARGETDURATION:2\n"
                                             "#EXT-X-MEDIA-SEQUENCE:7\n"
                                             "#EXT-X-INDEPENDENT-SEGMENTS\n"
                                             "#EXT-X-MAP:URI=\"init_0.mp4\"\n"
                                             "#EXTINF:2.000000,\n"
                                             "seg_7.m4s\n"
                                             "#EXTINF:1.966667,\n"
                                             "seg_8.m4s\n"
                                             "#EXT-X-ENDLIST\n";

// 2026-09-29T09:06:17.431Z
constexpr std::int64_t kStartMillis = 1'790'672'777'431;

core::WallTime at(std::int64_t millis) {
    return core::WallTime{std::chrono::milliseconds{millis}};
}

TEST(ParseMediaPlaylist, ReadsWhatFfmpegWrites) {
    const auto playlist = live::parse_media_playlist(kFfmpegPlaylist);
    ASSERT_TRUE(playlist);
    EXPECT_EQ(playlist->target_seconds, 2U);
    EXPECT_EQ(playlist->media_sequence, 7U);
    EXPECT_TRUE(playlist->ended);
    ASSERT_EQ(playlist->segments.size(), 2U);
    EXPECT_EQ(playlist->segments[0].uri, "seg_7.m4s");
    EXPECT_EQ(playlist->segments[0].duration, Micros{2'000'000});
    EXPECT_EQ(playlist->segments[0].init, "init_0.mp4");
    EXPECT_EQ(playlist->segments[1].duration, Micros{1'966'667});
    EXPECT_EQ(playlist->segments[1].init, "init_0.mp4");
}

TEST(ParseMediaPlaylist, AWindowWithoutEndlistIsNotEnded) {
    const auto playlist = live::parse_media_playlist(
        "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:2.0,\nseg_0.m4s\n");
    ASSERT_TRUE(playlist);
    EXPECT_FALSE(playlist->ended);
}

TEST(ParseMediaPlaylist, TagsApplyToTheSegmentThatFollowsThem) {
    const auto playlist = live::parse_media_playlist(
        "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:4\n"
        "#EXT-X-DISCONTINUITY-SEQUENCE:3\n"
        "#EXT-X-MAP:URI=\"init_0.mp4\"\n#EXTINF:2.0,\nseg_4.m4s\n"
        "#EXT-X-DISCONTINUITY\n#EXT-X-MAP:URI=\"init_1.mp4\"\n"
        "#EXT-X-PROGRAM-DATE-TIME:2026-09-29T09:06:17.431Z\n#EXTINF:2.0,\nseg_5.m4s\n"
        "#EXTINF:2.0,\nseg_6.m4s\n");
    ASSERT_TRUE(playlist);
    EXPECT_EQ(playlist->discontinuity_sequence, 3U);
    ASSERT_EQ(playlist->segments.size(), 3U);
    EXPECT_FALSE(playlist->segments[0].discontinuity);
    EXPECT_EQ(playlist->segments[0].init, "init_0.mp4");
    EXPECT_TRUE(playlist->segments[1].discontinuity);
    EXPECT_EQ(playlist->segments[1].init, "init_1.mp4");
    EXPECT_FALSE(playlist->segments[2].discontinuity);
    EXPECT_EQ(playlist->segments[2].init, "init_1.mp4");
    EXPECT_FALSE(playlist->segments[0].program_date_time);
    EXPECT_EQ(playlist->segments[1].program_date_time, at(kStartMillis));
    EXPECT_FALSE(playlist->segments[2].program_date_time);
}

TEST(ParseMediaPlaylist, AcceptsEverySpellingOfUtcFfmpegAndWeUse) {
    for (const char* stamp : {"2026-09-29T09:06:17.431Z", "2026-09-29T09:06:17.431+0000",
                              "2026-09-29T09:06:17.431+00:00"}) {
        const auto playlist = live::parse_media_playlist(
            std::string("#EXTM3U\n#EXT-X-PROGRAM-DATE-TIME:") + stamp + "\n#EXTINF:2,\ns.m4s\n");
        ASSERT_TRUE(playlist) << stamp;
        EXPECT_EQ(playlist->segments[0].program_date_time, at(kStartMillis)) << stamp;
    }
}

TEST(ParseMediaPlaylist, ADateTimeInAnotherZoneIsNoTimeRatherThanAWrongOne) {
    const auto playlist = live::parse_media_playlist(
        "#EXTM3U\n#EXT-X-PROGRAM-DATE-TIME:2026-09-29T09:06:17.431+02:00\n#EXTINF:2,\ns.m4s\n");
    ASSERT_TRUE(playlist);
    EXPECT_FALSE(playlist->segments[0].program_date_time);
}

TEST(ParseMediaPlaylist, UnknownTagsAreIgnoredAsPlayersDo) {
    const auto playlist = live::parse_media_playlist(
        "#EXTM3U\n#EXT-X-FUTURE:1\n#EXT-X-TARGETDURATION:2\n#EXTINF:2,\ns.m4s\n");
    ASSERT_TRUE(playlist);
    EXPECT_EQ(playlist->segments.size(), 1U);
}

TEST(ParseMediaPlaylist, RefusesWhatIsNotAPlaylist) {
    EXPECT_EQ(live::parse_media_playlist("").error(), PlaylistError::NotAPlaylist);
    EXPECT_EQ(live::parse_media_playlist("<html>").error(), PlaylistError::NotAPlaylist);
    EXPECT_EQ(live::parse_media_playlist("#EXT-X-VERSION:7\n#EXTINF:2,\ns\n").error(),
              PlaylistError::NotAPlaylist);
}

TEST(ParseMediaPlaylist, RefusesValuesThatDoNotParse) {
    for (const char* bad :
         {"#EXT-X-TARGETDURATION:two\n", "#EXT-X-TARGETDURATION:-2\n", "#EXT-X-MEDIA-SEQUENCE:1x\n",
          "#EXT-X-MEDIA-SEQUENCE:\n", "#EXT-X-DISCONTINUITY-SEQUENCE:99999999999999999999999\n",
          "#EXT-X-MAP:BYTERANGE=\"1@0\"\n", "#EXT-X-MAP:URI=\"unclosed\n", "#EXTINF:abc,\ns.m4s\n",
          "#EXTINF:2.,\ns.m4s\n", "#EXTINF:2.0,\n#EXTINF:2.0,\ns.m4s\n"}) {
        EXPECT_EQ(live::parse_media_playlist(std::string("#EXTM3U\n") + bad).error(),
                  PlaylistError::Malformed)
            << bad;
    }
}

TEST(ParseMediaPlaylist, ASegmentUriWithoutItsDurationOrTheOtherWayRoundIsMalformed) {
    EXPECT_EQ(live::parse_media_playlist("#EXTM3U\nseg_0.m4s\n").error(), PlaylistError::Malformed);
    EXPECT_EQ(live::parse_media_playlist("#EXTM3U\n#EXTINF:2.0,\n").error(),
              PlaylistError::Malformed);
}

TEST(ParseMediaPlaylist, RefusesMoreSegmentsThanAnyWindowWeWrite) {
    std::string text = "#EXTM3U\n";
    for (std::size_t i = 0; i <= live::kMaxSegments; ++i) {
        text += "#EXTINF:2,\ns" + std::to_string(i) + ".m4s\n";
    }
    EXPECT_EQ(live::parse_media_playlist(text).error(), PlaylistError::TooLong);
}

TEST(ParseMediaPlaylist, ToleratesCarriageReturns) {
    const auto playlist = live::parse_media_playlist(
        "#EXTM3U\r\n#EXT-X-TARGETDURATION:2\r\n#EXTINF:2.5,\r\ns.m4s\r\n");
    ASSERT_TRUE(playlist);
    EXPECT_EQ(playlist->segments[0].uri, "s.m4s");
    EXPECT_EQ(playlist->segments[0].duration, Micros{2'500'000});
}

MediaPlaylist window_of_two() {
    return {.target_seconds = 2,
            .media_sequence = 12,
            .discontinuity_sequence = 1,
            .ended = false,
            .segments = {{.uri = "seg_12.m4s",
                          .duration = Micros{2'000'000},
                          .init = "init_0.mp4",
                          .discontinuity = false,
                          .program_date_time = at(kStartMillis)},
                         {.uri = "seg_13.m4s",
                          .duration = Micros{1'999'000},
                          .init = "init_1.mp4",
                          .discontinuity = true,
                          .program_date_time = at(kStartMillis + 2000)}}};
}

TEST(RenderMediaPlaylist, WritesTheTagsAPlayerNeedsForALiveWindow) {
    EXPECT_EQ(live::render_media_playlist(window_of_two()),
              "#EXTM3U\n"
              "#EXT-X-VERSION:7\n"
              "#EXT-X-TARGETDURATION:2\n"
              "#EXT-X-MEDIA-SEQUENCE:12\n"
              "#EXT-X-DISCONTINUITY-SEQUENCE:1\n"
              "#EXT-X-INDEPENDENT-SEGMENTS\n"
              "#EXT-X-MAP:URI=\"init_0.mp4\"\n"
              "#EXT-X-PROGRAM-DATE-TIME:2026-09-29T09:06:17.431Z\n"
              "#EXTINF:2.000000,\n"
              "seg_12.m4s\n"
              "#EXT-X-DISCONTINUITY\n"
              "#EXT-X-MAP:URI=\"init_1.mp4\"\n"
              "#EXT-X-PROGRAM-DATE-TIME:2026-09-29T09:06:19.431Z\n"
              "#EXTINF:1.999000,\n"
              "seg_13.m4s\n");
}

TEST(RenderMediaPlaylist, EndsAnEndedPlaylistWithEndlistAndNothingAfter) {
    MediaPlaylist playlist = window_of_two();
    playlist.ended = true;
    EXPECT_TRUE(live::render_media_playlist(playlist).ends_with("seg_13.m4s\n#EXT-X-ENDLIST\n"));
}

TEST(RenderMediaPlaylist, OmitsTheDiscontinuitySequenceWhileItIsZero) {
    MediaPlaylist playlist = window_of_two();
    playlist.discontinuity_sequence = 0;
    EXPECT_EQ(live::render_media_playlist(playlist).find("DISCONTINUITY-SEQUENCE"),
              std::string::npos);
}

TEST(RenderMediaPlaylist, RepeatsNoMapWhileTheInitSegmentStaysTheSame) {
    MediaPlaylist playlist = window_of_two();
    playlist.segments[1].init = "init_0.mp4";
    const std::string text = live::render_media_playlist(playlist);
    EXPECT_EQ(text.find("#EXT-X-MAP"), text.rfind("#EXT-X-MAP"));
}

TEST(RenderMediaPlaylist, ParsesBackToTheSamePlaylist) {
    MediaPlaylist original = window_of_two();
    original.ended = true;
    const auto parsed = live::parse_media_playlist(live::render_media_playlist(original));
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->target_seconds, original.target_seconds);
    EXPECT_EQ(parsed->media_sequence, original.media_sequence);
    EXPECT_EQ(parsed->discontinuity_sequence, original.discontinuity_sequence);
    EXPECT_EQ(parsed->ended, original.ended);
    ASSERT_EQ(parsed->segments.size(), original.segments.size());
    for (std::size_t i = 0; i < original.segments.size(); ++i) {
        EXPECT_EQ(parsed->segments[i].uri, original.segments[i].uri);
        EXPECT_EQ(parsed->segments[i].duration, original.segments[i].duration);
        EXPECT_EQ(parsed->segments[i].init, original.segments[i].init);
        EXPECT_EQ(parsed->segments[i].discontinuity, original.segments[i].discontinuity);
        EXPECT_EQ(parsed->segments[i].program_date_time, original.segments[i].program_date_time);
    }
}

TEST(RenderMediaPlaylist, KeepsMillisecondsAndDropsFinerTimeInTheDateTime) {
    MediaPlaylist playlist = window_of_two();
    playlist.segments[0].program_date_time =
        core::WallTime{} + std::chrono::duration_cast<core::WallTime::duration>(
                               std::chrono::microseconds{1'790'672'777'431'999});
    EXPECT_NE(live::render_media_playlist(playlist).find("2026-09-29T09:06:17.431Z"),
              std::string::npos);
}

} // namespace
