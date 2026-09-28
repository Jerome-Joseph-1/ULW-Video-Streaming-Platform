#include "core/models/ladder.hpp"
#include "core/ports/transcoder.hpp"

#include "command.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <vector>

namespace {

using core::ports::FrameRate;
using core::ports::MediaInfo;
using infra::ffmpeg::Args;
using infra::ffmpeg::gop_frames;
using infra::ffmpeg::parse_probe;
using infra::ffmpeg::transcode_args;

MediaInfo media(std::uint32_t height, bool audio = true, FrameRate rate = {.num = 30, .den = 1}) {
    return MediaInfo{.width = height * 16 / 9,
                     .height = height,
                     .frame_rate = rate,
                     .duration = core::Millis{60'000},
                     .has_audio = audio};
}

// The value that follows `flag`, or "" when the flag is absent.
std::string after(const Args& args, std::string_view flag) {
    const auto it = std::ranges::find(args, flag);
    return it == args.end() || it + 1 == args.end() ? std::string() : *(it + 1);
}

TEST(Gop, IsFourSecondsOfFramesRoundedFromTheRationalRate) {
    EXPECT_EQ(gop_frames({.num = 30000, .den = 1001}), 120U); // 119.88
    EXPECT_EQ(gop_frames({.num = 24000, .den = 1001}), 96U);  // 95.904
    EXPECT_EQ(gop_frames({.num = 60000, .den = 1001}), 240U); // 239.76
    EXPECT_EQ(gop_frames({.num = 25, .den = 1}), 100U);
    EXPECT_EQ(gop_frames({.num = 50, .den = 1}), 200U);
    EXPECT_EQ(gop_frames({.num = 15, .den = 2}), 30U);
}

TEST(Gop, RoundsHalfwayUpAndNeverReachesZero) {
    // 4 x 5/8 = 2.5 frames.
    EXPECT_EQ(gop_frames({.num = 5, .den = 8}), 3U);
    EXPECT_EQ(gop_frames({.num = 1, .den = 1000}), 1U);
}

constexpr std::string_view kFullGraph =
    "[0:v]split=3[v1][v2][v3];[v1]scale=-2:1080[v1o];[v2]scale=-2:720[v2o];[v3]scale=-2:360[v3o]";

// The command in the spec, token for token, for a 1080p30 source with audio.
TEST(TranscodeArgs, ReproduceTheSpecCommandForAFullLadder) {
    const auto ladder = core::choose_ladder(1080);
    const Args expected{"ffmpeg",
                        "-nostdin",
                        "-hide_banner",
                        "-loglevel",
                        "warning",
                        "-y",
                        "-format_whitelist",
                        "mov,matroska,mpegts,avi,flv,asf,mpeg,ogg",
                        "-i",
                        "in",
                        "-filter_complex",
                        std::string(kFullGraph),
                        "-map",
                        "[v1o]",
                        "-c:v:0",
                        "libx264",
                        "-b:v:0",
                        "5000k",
                        "-maxrate:v:0",
                        "5350k",
                        "-bufsize:v:0",
                        "7500k",
                        "-map",
                        "[v2o]",
                        "-c:v:1",
                        "libx264",
                        "-b:v:1",
                        "2800k",
                        "-maxrate:v:1",
                        "2996k",
                        "-bufsize:v:1",
                        "4200k",
                        "-map",
                        "[v3o]",
                        "-c:v:2",
                        "libx264",
                        "-b:v:2",
                        "800k",
                        "-maxrate:v:2",
                        "856k",
                        "-bufsize:v:2",
                        "1200k",
                        "-map",
                        "a:0",
                        "-map",
                        "a:0",
                        "-map",
                        "a:0",
                        "-c:a",
                        "aac",
                        "-b:a",
                        "128k",
                        "-ac",
                        "2",
                        "-ar",
                        "48000",
                        "-preset",
                        "veryfast",
                        "-profile:v",
                        "main",
                        "-level",
                        "4.0",
                        "-pix_fmt",
                        "yuv420p",
                        "-sc_threshold",
                        "0",
                        "-g",
                        "120",
                        "-keyint_min",
                        "120",
                        "-force_key_frames",
                        "expr:gte(t,n_forced*4)",
                        "-hls_time",
                        "4",
                        "-hls_playlist_type",
                        "vod",
                        "-hls_segment_type",
                        "fmp4",
                        "-hls_flags",
                        "independent_segments",
                        "-master_pl_name",
                        "master.m3u8",
                        "-var_stream_map",
                        "v:0,a:0,name:1080p v:1,a:1,name:720p v:2,a:2,name:360p",
                        "-hls_segment_filename",
                        "out/%v/seg_%05d.m4s",
                        "-threads",
                        "4",
                        "-progress",
                        "pipe:1",
                        "-nostats",
                        "out/%v/index.m3u8"};
    EXPECT_EQ(transcode_args("ffmpeg", "in", "out", media(1080), ladder, 4), expected);
}

TEST(TranscodeArgs, TheGopFollowsTheSourceRate) {
    const auto ladder = core::choose_ladder(720);
    const Args args = transcode_args("ffmpeg", "in", "out",
                                     media(720, true, {.num = 30000, .den = 1001}), ladder, 2);
    EXPECT_EQ(after(args, "-g"), "120");
    EXPECT_EQ(after(args, "-keyint_min"), "120");
    const Args pal =
        transcode_args("ffmpeg", "in", "out", media(720, true, {.num = 25, .den = 1}), ladder, 2);
    EXPECT_EQ(after(pal, "-g"), "100");
}

TEST(TranscodeArgs, A480pSourceIsEncodedAt360pOnly) {
    const auto ladder = core::choose_ladder(480);
    const Args args = transcode_args("ffmpeg", "in", "out", media(480), ladder, 2);
    EXPECT_EQ(after(args, "-filter_complex"), "[0:v]split=1[v1];[v1]scale=-2:360[v1o]");
    EXPECT_EQ(after(args, "-var_stream_map"), "v:0,a:0,name:360p");
    EXPECT_EQ(std::ranges::count(args, "-map"), 2);
    EXPECT_EQ(after(args, "-b:v:0"), "800k");
    EXPECT_EQ(std::ranges::find(args, "-b:v:1"), args.end());
}

TEST(TranscodeArgs, NoRungIsScaledAboveTheSource) {
    const auto ladder = core::choose_ladder(720);
    const std::string graph =
        after(transcode_args("ffmpeg", "in", "out", media(720), ladder, 2), "-filter_complex");
    EXPECT_EQ(graph, "[0:v]split=2[v1][v2];[v1]scale=-2:720[v1o];[v2]scale=-2:360[v2o]");
    EXPECT_EQ(graph.find("1080"), std::string::npos);
}

TEST(TranscodeArgs, ASilentSourceMapsNoAudio) {
    const auto ladder = core::choose_ladder(720);
    const Args args = transcode_args("ffmpeg", "in", "out", media(720, false), ladder, 2);
    EXPECT_EQ(std::ranges::find(args, "a:0"), args.end());
    EXPECT_EQ(std::ranges::find(args, "-c:a"), args.end());
    EXPECT_EQ(after(args, "-var_stream_map"), "v:0,name:720p v:1,name:360p");
}

TEST(TranscodeArgs, NeverMixConstantQualityWithABitrate) {
    const Args args =
        transcode_args("ffmpeg", "in", "out", media(1080), core::choose_ladder(1080), 2);
    EXPECT_EQ(std::ranges::find(args, "-crf"), args.end());
    EXPECT_EQ(std::ranges::find(args, "-qp"), args.end());
}

TEST(TranscodeArgs, PassesTheThreadCountExplicitly) {
    const Args args =
        transcode_args("ffmpeg", "in", "out", media(360), core::choose_ladder(360), 3);
    EXPECT_EQ(after(args, "-threads"), "3");
}

TEST(TranscodeArgs, RungsBelow360pGetTheirOwnHeightAndName) {
    const auto ladder = core::choose_ladder(240);
    const Args args = transcode_args("ffmpeg", "in", "out", media(240), ladder, 1);
    EXPECT_EQ(after(args, "-filter_complex"), "[0:v]split=1[v1];[v1]scale=-2:240[v1o]");
    EXPECT_EQ(after(args, "-var_stream_map"), "v:0,a:0,name:240p");
    EXPECT_EQ(after(args, "-maxrate:v:0"), "570k");
    EXPECT_EQ(after(args, "-bufsize:v:0"), "799k");
}

// What ffprobe 6.1 prints for probe_args on a 720p clip with audio.
constexpr std::string_view kProbed = "codec_type=video\n"
                                     "width=1280\n"
                                     "height=720\n"
                                     "r_frame_rate=30000/1001\n"
                                     "codec_type=audio\n"
                                     "r_frame_rate=0/0\n"
                                     "duration=6.006000\n";

TEST(ParseProbe, ReadsDimensionsRateDurationAndAudio) {
    const auto info = parse_probe(kProbed);
    ASSERT_TRUE(info) << info.error();
    EXPECT_EQ(info->width, 1280U);
    EXPECT_EQ(info->height, 720U);
    EXPECT_EQ(info->frame_rate, (FrameRate{.num = 30000, .den = 1001}));
    EXPECT_EQ(info->duration, core::Millis{6006});
    EXPECT_TRUE(info->has_audio);
}

TEST(ParseProbe, AQuarterTurnSwapsTheDisplayedDimensions) {
    const auto info = parse_probe("codec_type=video\nwidth=1920\nheight=1080\n"
                                  "r_frame_rate=30/1\nrotation=-90\nduration=2.5\n");
    ASSERT_TRUE(info) << info.error();
    EXPECT_EQ(info->width, 1080U);
    EXPECT_EQ(info->height, 1920U);
    EXPECT_FALSE(info->has_audio);
    EXPECT_EQ(info->duration, core::Millis{2500});
    const auto upside_down = parse_probe("codec_type=video\nwidth=1920\nheight=1080\n"
                                         "r_frame_rate=30/1\nrotation=180\nduration=2\n");
    ASSERT_TRUE(upside_down);
    EXPECT_EQ(upside_down->height, 1080U);
}

TEST(ParseProbe, TheFirstVideoStreamIsTheOneDescribed) {
    const auto info = parse_probe("codec_type=audio\nr_frame_rate=0/0\n"
                                  "codec_type=video\nwidth=640\nheight=360\nr_frame_rate=25/1\n"
                                  "codec_type=video\nwidth=1920\nheight=1080\nr_frame_rate=25/1\n"
                                  "duration=1.000000\n");
    ASSERT_TRUE(info) << info.error();
    EXPECT_EQ(info->height, 360U);
    EXPECT_TRUE(info->has_audio);
}

TEST(ParseProbe, RefusesWhatCannotBeTranscoded) {
    EXPECT_EQ(parse_probe("codec_type=audio\nduration=3.0\n").error(), "no video stream");
    EXPECT_EQ(parse_probe("codec_type=video\nwidth=N/A\nheight=N/A\nr_frame_rate=25/1\n"
                          "duration=1.0\n")
                  .error(),
              "video stream has no dimensions");
    EXPECT_EQ(parse_probe("codec_type=video\nwidth=64\nheight=64\nr_frame_rate=0/0\n"
                          "duration=1.0\n")
                  .error(),
              "video stream has no frame rate");
    EXPECT_EQ(parse_probe("codec_type=video\nwidth=64\nheight=64\nr_frame_rate=25/1\n"
                          "duration=N/A\n")
                  .error(),
              "no duration");
    EXPECT_EQ(parse_probe("codec_type=video\nwidth=64\nheight=64\nr_frame_rate=25/1\n"
                          "duration=-1.0\n")
                  .error(),
              "no duration");
    EXPECT_FALSE(parse_probe(""));
}

TEST(Demuxers, EveryInputIsOpenedWithAClosedListOfThem) {
    const auto ladder = core::choose_ladder(720);
    const auto source_first = [](const Args& args, std::string_view input) {
        const auto list = std::ranges::find(args, "-format_whitelist");
        return list != args.end() && list < std::ranges::find(args, input);
    };
    const Args probe = infra::ffmpeg::probe_args("ffprobe", "in");
    const Args encode = transcode_args("ffmpeg", "in", "out", media(720), ladder, 2);
    EXPECT_EQ(after(probe, "-format_whitelist"), "mov,matroska,mpegts,avi,flv,asf,mpeg,ogg");
    EXPECT_EQ(after(encode, "-format_whitelist"), after(probe, "-format_whitelist"));
    EXPECT_TRUE(source_first(probe, "in"));
    EXPECT_TRUE(source_first(encode, "in"));
    // Nothing that reads a list of other files is on it.
    for (const std::string_view format : {"hls", "dash", "imf", "concat"}) {
        EXPECT_EQ(after(probe, "-format_whitelist").find(format), std::string::npos) << format;
    }

    const Args keyframes = infra::ffmpeg::keyframe_args("ffprobe", "out/360p/index.m3u8");
    const Args decode = infra::ffmpeg::decode_args("ffmpeg", "out/master.m3u8");
    EXPECT_EQ(after(keyframes, "-format_whitelist"), "hls,mov");
    EXPECT_EQ(after(decode, "-format_whitelist"), "hls,mov");
    EXPECT_TRUE(source_first(keyframes, "out/360p/index.m3u8"));
    EXPECT_TRUE(source_first(decode, "out/master.m3u8"));
}

TEST(ParseKeyframes, ReadsOneTimePerLine) {
    EXPECT_EQ(infra::ffmpeg::parse_keyframes("0.066000\n4.070000\n8.074000\n"),
              (std::vector<std::string>{"0.066000", "4.070000", "8.074000"}));
    EXPECT_FALSE(infra::ffmpeg::parse_keyframes("0.066000\nN/A\n"));
}

// As ffmpeg 6.1 writes them for the spec command.
constexpr std::string_view kMedia = "#EXTM3U\n"
                                    "#EXT-X-VERSION:7\n"
                                    "#EXT-X-TARGETDURATION:4\n"
                                    "#EXT-X-MEDIA-SEQUENCE:0\n"
                                    "#EXT-X-PLAYLIST-TYPE:VOD\n"
                                    "#EXT-X-INDEPENDENT-SEGMENTS\n"
                                    "#EXT-X-MAP:URI=\"init_1.mp4\"\n"
                                    "#EXTINF:4.004000,\n"
                                    "seg_00000.m4s\n"
                                    "#EXTINF:1.968633,\n"
                                    "seg_00001.m4s\n"
                                    "#EXT-X-ENDLIST\n";
constexpr std::string_view kMaster =
    "#EXTM3U\n"
    "#EXT-X-VERSION:7\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=3220800,RESOLUTION=1280x720,CODECS=\"avc1.4d4028,mp4a.40.2\"\n"
    "720p/index.m3u8\n"
    "\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=1020800,RESOLUTION=640x360,CODECS=\"avc1.4d4028,mp4a.40.2\"\n"
    "360p/index.m3u8\n";

TEST(CheckPlaylists, AcceptWhatFfmpegWrites) {
    EXPECT_EQ(infra::ffmpeg::check_media_playlist(kMedia), std::nullopt);
    EXPECT_EQ(infra::ffmpeg::check_master_playlist(kMaster, core::choose_ladder(720)),
              std::nullopt);
}

TEST(CheckPlaylists, AMediaPlaylistNeedsVersion7AMapAndAnEnd) {
    for (const std::string_view tag : {"#EXT-X-VERSION:7", "#EXT-X-MAP:URI=", "#EXT-X-ENDLIST"}) {
        std::string text(kMedia);
        text.erase(text.find(tag), tag.size());
        EXPECT_TRUE(infra::ffmpeg::check_media_playlist(text)) << tag;
    }
}

TEST(CheckPlaylists, TheMasterMustNameEveryRungWithCodecs) {
    EXPECT_TRUE(infra::ffmpeg::check_master_playlist(kMaster, core::choose_ladder(1080)));
    std::string no_codecs(kMaster);
    no_codecs.erase(no_codecs.find("CODECS"), 6);
    EXPECT_TRUE(infra::ffmpeg::check_master_playlist(no_codecs, core::choose_ladder(720)));
    std::string renamed(kMaster);
    renamed.replace(renamed.find("360p/"), 4, "240p");
    EXPECT_TRUE(infra::ffmpeg::check_master_playlist(renamed, core::choose_ladder(720)));
}

} // namespace
