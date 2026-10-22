#include "core/models/caption_language.hpp"
#include "core/util/hls.hpp"
#include "core/util/webvtt.hpp"

#include <array>
#include <gtest/gtest.h>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A video's captions (ADR-0139): the WebVTT check, the language tag, and how the master and
// the subtitles playlist list them.
namespace {

using core::CaptionLanguage;
using core::webvtt::check;
using core::webvtt::Problem;

constexpr std::string_view kFile = "WEBVTT\n"
                                   "\n"
                                   "00:00.000 --> 00:02.500\n"
                                   "Hello.\n"
                                   "\n"
                                   "intro-2\n"
                                   "00:02.500 --> 00:04.000 align:start position:10%\n"
                                   "<v Alice>Two lines,\n"
                                   "of text.\n";

std::pair<Problem, std::size_t> at(Problem problem, std::size_t line) {
    return {problem, line};
}

// The problem `text` is refused for, and on which line; line 0 when it passes.
std::pair<Problem, std::size_t> refusal(std::string_view text) {
    const auto r = check(text);
    EXPECT_FALSE(r) << text;
    return r ? at(Problem::Block, 0) : at(r.error().problem, r.error().line);
}

TEST(WebVtt, AFileOfCuesPasses) {
    EXPECT_TRUE(check(kFile));
    // The signature alone is a file with no cues.
    EXPECT_TRUE(check("WEBVTT"));
    EXPECT_TRUE(check("WEBVTT\n"));
}

TEST(WebVtt, TheHeaderMayCarryATitleAndLinesOfItsOwn) {
    EXPECT_TRUE(check("\xEF\xBB\xBFWEBVTT\n\n00:01.000 --> 00:02.000\nhi\n"));
    EXPECT_TRUE(check("WEBVTT - Trip, in English\n\n00:01.000 --> 00:02.000\nhi\n"));
    EXPECT_TRUE(check("WEBVTT\tx\n\n00:01.000 --> 00:02.000\nhi\n"));
    EXPECT_TRUE(check("WEBVTT\nX-TIMESTAMP-MAP=MPEGTS:900000,LOCAL:00:00:00.000\n\n00:01.000 --> "
                      "00:02.000\nhi\n"));
}

TEST(WebVtt, EveryLineTerminatorIsOne) {
    EXPECT_TRUE(check("WEBVTT\r\n\r\n00:01.000 --> 00:02.000\r\nhi\r\n\r\n00:02.000 --> "
                      "00:03.000\r\nyou\r\n"));
    EXPECT_TRUE(check("WEBVTT\r\r00:01.000 --> 00:02.000\rhi\r"));
    // Counted once each, CR LF included: the third line is the bad timing.
    EXPECT_EQ(refusal("WEBVTT\r\n\r\n00:01.00 --> 00:02.000\r\n"), at(Problem::Timing, 3));
    EXPECT_EQ(refusal("WEBVTT\r\r00:01.00 --> 00:02.000\r"), at(Problem::Timing, 3));
}

TEST(WebVtt, TimesTakeHoursAndSettings) {
    EXPECT_TRUE(check("WEBVTT\n\n01:00:00.000 --> 01:00:01.500\nan hour in\n"));
    EXPECT_TRUE(check("WEBVTT\n\n100:00:00.000 --> 100:00:01.000\na long one\n"));
    EXPECT_TRUE(check("WEBVTT\n\n00:59.999\t-->\t01:00.000 line:0\nedge\n"));
    // Two cues may start together, and overlap.
    EXPECT_TRUE(check("WEBVTT\n\n00:01.000 --> 00:05.000\na\n\n00:01.000 --> 00:02.000\nb\n"));
}

TEST(WebVtt, NotesStylesAndRegionsAreBlocksOfTheirOwn) {
    EXPECT_TRUE(check("WEBVTT\n\nSTYLE\n::cue { color: yellow }\n\nREGION\nid:r1 width:40%\n\n"
                      "NOTE a comment\n\n00:01.000 --> 00:02.000 region:r1\nhi\n\nNOTE\n"
                      "after the cue\n"));
    // Past the first cue a style is not one: players take it for a cue with no timing.
    EXPECT_EQ(refusal("WEBVTT\n\n00:01.000 --> 00:02.000\nhi\n\nSTYLE\n::cue {}\n"),
              at(Problem::Block, 6));
    EXPECT_EQ(refusal("WEBVTT\n\nNOTE 00:01.000 --> 00:02.000\n"), at(Problem::Block, 3));
    EXPECT_EQ(refusal("WEBVTT\n\nNOTE\nsee 00:01.000 --> 00:02.000\n"), at(Problem::Block, 4));
}

TEST(WebVtt, AFileThatIsNotUtf8IsRefused) {
    EXPECT_TRUE(check("WEBVTT\n\n00:01.000 --> 00:02.000\nÇa va, 日本語, 🎬\n"));
    for (const std::string_view bad :
         {std::string_view{"\xC0\x80"}, std::string_view{"\xED\xA0\x80"},
          std::string_view{"\xF4\x90\x80\x80"}, std::string_view{"\xF5\x80\x80\x80"},
          std::string_view{"\xE2\x82"}, std::string_view{"\x80"}, std::string_view{"a\0b", 3}}) {
        const std::string text = "WEBVTT\n\n00:01.000 --> 00:02.000\n" + std::string(bad) + "\n";
        EXPECT_EQ(refusal(text), at(Problem::Encoding, 4));
    }
    EXPECT_EQ(refusal("WEBVTT\r\n\xFF"), at(Problem::Encoding, 2));
}

TEST(WebVtt, AFileWithoutTheSignatureIsRefused) {
    for (const std::string_view text :
         {std::string_view{}, std::string_view{"webvtt\n"}, std::string_view{"WEBVTTX\n"},
          std::string_view{" WEBVTT\n"}, std::string_view{"1\n00:01.000 --> 00:02.000\nhi\n"},
          std::string_view{"\xEF\xBB\xBF\xEF\xBB\xBFWEBVTT\n"}}) {
        EXPECT_EQ(refusal(text), at(Problem::Header, 1)) << text;
    }
    // A header line a player would read as a cue.
    EXPECT_EQ(refusal("WEBVTT\n00:01.000 --> 00:02.000\nhi\n"), at(Problem::Header, 2));
}

TEST(WebVtt, ATimingLineMustBeExact) {
    for (const std::string_view timing :
         {"00:01.000-->00:02.000", "00:01.000 -->00:02.000", "0:01.000 --> 00:02.000",
          "1:00:01.000 --> 1:00:02.000", "00:60.000 --> 01:00.000", "60:00.000 --> 61:00.000",
          "00:60:00.000 --> 01:00:00.000", "00:01.00 --> 00:02.000", "00:01,000 --> 00:02.000",
          "00:01.000 --> 00:02.000x", "00:01.000 --> ", "00:02.000 --> 00:02.000",
          "00:03.000 --> 00:02.000", "12345678901:00:00.000 --> x", "a --> b"}) {
        const std::string text = "WEBVTT\n\ncue\n" + std::string(timing) + "\nhi\n";
        EXPECT_EQ(refusal(text), at(Problem::Timing, 4)) << timing;
    }
}

TEST(WebVtt, CuesComeInTheOrderTheyStart) {
    EXPECT_EQ(refusal("WEBVTT\n\n00:05.000 --> 00:06.000\nb\n\n00:01.000 --> 00:02.000\na\n"),
              at(Problem::Order, 6));
}

TEST(WebVtt, ABlockIsACueOrItIsRefused) {
    // Neither of its first two lines is a timing.
    EXPECT_EQ(refusal("WEBVTT\n\nhello\nthere\n00:01.000 --> 00:02.000\n"), at(Problem::Block, 3));
    EXPECT_EQ(refusal("WEBVTT\n\njust an id\n"), at(Problem::Block, 3));
    // An arrow in a cue's text ends the cue for a parser and starts another.
    EXPECT_EQ(refusal("WEBVTT\n\n00:01.000 --> 00:02.000\nhi\n--> there\n"), at(Problem::Block, 5));
}

TEST(WebVtt, ProblemsHaveNames) {
    EXPECT_EQ(core::webvtt::to_string(Problem::Encoding), "encoding");
    EXPECT_EQ(core::webvtt::to_string(Problem::Header), "header");
    EXPECT_EQ(core::webvtt::to_string(Problem::Timing), "timing");
    EXPECT_EQ(core::webvtt::to_string(Problem::Order), "order");
    EXPECT_EQ(core::webvtt::to_string(Problem::Block), "block");
}

TEST(CaptionLanguage, ATagIsKeptLowercase) {
    for (const auto& [text, kept] :
         std::array<std::pair<std::string_view, std::string_view>, 6>{{{"en", "en"},
                                                                       {"EN-us", "en-us"},
                                                                       {"pt-BR", "pt-br"},
                                                                       {"zh-Hant-TW", "zh-hant-tw"},
                                                                       {"es-419", "es-419"},
                                                                       {"yue", "yue"}}}) {
        const auto language = CaptionLanguage::parse(text);
        ASSERT_TRUE(language) << text;
        EXPECT_EQ(language->view(), kept);
        EXPECT_EQ(language->str(), kept);
    }
    EXPECT_EQ(*CaptionLanguage::parse("en-US"), *CaptionLanguage::parse("en-us"));
    EXPECT_LT(*CaptionLanguage::parse("de"), *CaptionLanguage::parse("en"));
}

TEST(CaptionLanguage, AnythingElseIsRefused) {
    EXPECT_EQ(CaptionLanguage::parse("").error(), core::DomainError::EmptyIdentifier);
    for (const std::string_view text :
         {"e", "engl", "e1", "en-", "-en", "en--us", "en-toolongtag", "en_us", "en us",
          "index.m3u8", "../en", "en-ü", "1en", "enx-us-x-abcdefgh-abcdefgh-abcdefgh-a"}) {
        EXPECT_EQ(CaptionLanguage::parse(text).error(), core::DomainError::MalformedIdentifier)
            << text;
    }
    // 35 characters, the longest kept.
    EXPECT_TRUE(CaptionLanguage::parse("enx-us-x-abcdefgh-abcdefgh-abcdefgh"));
}

constexpr std::string_view kRoutedMaster = "#EXTM3U\n"
                                           "#EXT-X-VERSION:7\n"
                                           "#EXT-X-STREAM-INF:BANDWIDTH=1020800\n"
                                           "/api/v1/videos/v1/720p/index.m3u8\n"
                                           "#EXT-X-STREAM-INF:BANDWIDTH=580800\n"
                                           "/api/v1/videos/v1/360p/index.m3u8\n";

TEST(Subtitles, TheMasterListsEachLanguageInOneGroup) {
    const std::vector<core::hls::SubtitleTrack> tracks{
        {.language = "en", .uri = "/api/v1/videos/v1/captions/en/index.m3u8"},
        {.language = "fr", .uri = "/api/v1/videos/v1/captions/fr/index.m3u8"}};
    const auto out = core::hls::add_subtitles(kRoutedMaster, tracks);
    ASSERT_TRUE(out);
    EXPECT_EQ(*out, "#EXTM3U\n"
                    "#EXT-X-VERSION:7\n"
                    "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"subs\",NAME=\"en\",LANGUAGE=\"en\","
                    "DEFAULT=NO,AUTOSELECT=YES,URI=\"/api/v1/videos/v1/captions/en/index.m3u8\"\n"
                    "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"subs\",NAME=\"fr\",LANGUAGE=\"fr\","
                    "DEFAULT=NO,AUTOSELECT=YES,URI=\"/api/v1/videos/v1/captions/fr/index.m3u8\"\n"
                    "#EXT-X-STREAM-INF:BANDWIDTH=1020800,SUBTITLES=\"subs\"\n"
                    "/api/v1/videos/v1/720p/index.m3u8\n"
                    "#EXT-X-STREAM-INF:BANDWIDTH=580800,SUBTITLES=\"subs\"\n"
                    "/api/v1/videos/v1/360p/index.m3u8\n");
    // Without captions the master is as it was.
    const auto none = core::hls::add_subtitles(kRoutedMaster, {});
    ASSERT_TRUE(none);
    EXPECT_EQ(*none, kRoutedMaster);
}

TEST(Subtitles, AMasterWithAGroupOfItsOwnOrABadTrackIsRefused) {
    const std::vector<core::hls::SubtitleTrack> en{{.language = "en", .uri = "/c/en/index.m3u8"}};
    EXPECT_EQ(core::hls::add_subtitles(
                  "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1,SUBTITLES=\"s\"\n/a/index.m3u8\n", en)
                  .error(),
              core::hls::PlaylistError::Malformed);
    EXPECT_EQ(
        core::hls::add_subtitles("#EXTM3U\n#EXT-X-STREAM-INF:SUBTITLES=\"s\"\n/a/index.m3u8\n", en)
            .error(),
        core::hls::PlaylistError::Malformed);
    EXPECT_EQ(core::hls::add_subtitles("#EXTM3U\n\x01\n", en).error(),
              core::hls::PlaylistError::Malformed);
    for (const core::hls::SubtitleTrack& bad :
         {core::hls::SubtitleTrack{.language = "e\"n", .uri = "/u"},
          core::hls::SubtitleTrack{.language = "en", .uri = "/u\n#EXT-X-ENDLIST"}}) {
        EXPECT_EQ(core::hls::add_subtitles(kRoutedMaster, std::span(&bad, 1)).error(),
                  core::hls::PlaylistError::Malformed);
    }
    // A master with no variant still lists them, at its end.
    const auto bare = core::hls::add_subtitles("#EXTM3U\n", en);
    ASSERT_TRUE(bare);
    EXPECT_TRUE(bare->starts_with("#EXTM3U\n#EXT-X-MEDIA:TYPE=SUBTITLES,")) << *bare;
}

TEST(Subtitles, ThePlaylistIsOneSegmentAsLongAsTheVideo) {
    EXPECT_EQ(core::hls::subtitle_playlist("https://store/en.vtt?sig", core::Millis{5'250}),
              "#EXTM3U\n"
              "#EXT-X-VERSION:3\n"
              "#EXT-X-TARGETDURATION:6\n"
              "#EXT-X-MEDIA-SEQUENCE:0\n"
              "#EXT-X-PLAYLIST-TYPE:VOD\n"
              "#EXTINF:5.250,\n"
              "https://store/en.vtt?sig\n"
              "#EXT-X-ENDLIST\n");
    const std::string whole = core::hls::subtitle_playlist("u", core::Millis{6'000});
    EXPECT_NE(whole.find("#EXT-X-TARGETDURATION:6\n"), std::string::npos) << whole;
    EXPECT_NE(whole.find("#EXTINF:6.000,\n"), std::string::npos) << whole;
    // Never a target of 0, which no player takes.
    const std::string empty = core::hls::subtitle_playlist("u", core::Millis{0});
    EXPECT_NE(empty.find("#EXT-X-TARGETDURATION:1\n"), std::string::npos) << empty;
    EXPECT_NE(empty.find("#EXTINF:0.000,\n"), std::string::npos) << empty;
}

} // namespace
