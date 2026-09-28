// The adapter against the real ffmpeg and ffprobe, in the real sandbox, on generated clips.
#include "core/models/ladder.hpp"
#include "infra/ffmpeg/transcoder.hpp"
#include "os/system_clock.hpp"

#include "media_clips.hpp"
#include "process.hpp"
#include "support/temp_dir.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <iterator>
#include <stop_token>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using core::ports::MediaInfo;
using core::ports::TranscodeFailure;
using ulw::test::Clip;

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string text;
    std::getline(in, text, '\0');
    return text;
}

std::vector<std::string> entries(const fs::path& dir) {
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(dir)) {
        names.push_back(e.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

struct Recorder final : core::ports::ITranscodeProgress {
    std::vector<core::Millis> seen;
    std::function<void()> on_first;
    void on_progress(core::Millis encoded) noexcept override {
        seen.push_back(encoded);
        if (seen.size() == 1 && on_first) {
            on_first();
        }
    }
};

class TranscoderTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (ulw::test::run_process({"ffmpeg", "-version"}).exit_code != 0) {
            GTEST_SKIP() << "ffmpeg is not installed";
        }
        if (const auto refused =
                infra::ffmpeg::check_sandbox(ULW_SANDBOX_BIN, work_.path(), clock_)) {
            GTEST_SKIP() << "this host refuses the sandbox: " << *refused;
        }
    }

    // Probes and transcodes `clip` with the ladder chosen for it.
    MediaInfo transcode(const Clip& clip) {
        EXPECT_TRUE(ulw::test::make_clip(source(), clip));
        auto media = transcoder_.probe(source(), {});
        EXPECT_TRUE(media) << media.error().detail;
        ladder_ = core::choose_ladder(media->height);
        const auto stats = transcoder_.run(source(), out(), *media, ladder_, progress_, {});
        EXPECT_TRUE(stats) << stats.error().detail;
        return *media;
    }

    [[nodiscard]] fs::path source() const { return work_.path() / "source.mp4"; }
    [[nodiscard]] fs::path out() const { return work_.path() / "hls"; }

    static std::string search_path() {
        const char* path = std::getenv("PATH");
        return path == nullptr ? "/usr/bin:/bin" : path;
    }

    os::SystemClock clock_;
    ulw::test::TempDir work_{"ulw-transcode"};
    infra::ffmpeg::FfmpegTranscoder transcoder_{{.sandbox = ULW_SANDBOX_BIN,
                                                 .ffmpeg = "ffmpeg",
                                                 .ffprobe = "ffprobe",
                                                 .search_path = search_path(),
                                                 .threads = 2},
                                                clock_};
    std::vector<core::Rung> ladder_;
    Recorder progress_;
};

TEST_F(TranscoderTest, ProbeDescribesARealClip) {
    ASSERT_TRUE(ulw::test::make_clip(
        source(), {.size = "1280x720", .rate = "30000/1001", .seconds = 2, .audio = true}));
    const auto media = transcoder_.probe(source(), {});
    ASSERT_TRUE(media) << media.error().detail;
    EXPECT_EQ(media->width, 1280U);
    EXPECT_EQ(media->height, 720U);
    EXPECT_EQ(media->frame_rate, (core::ports::FrameRate{.num = 30000, .den = 1001}));
    EXPECT_NEAR(static_cast<double>(media->duration.count()), 2000.0, 100.0);
    EXPECT_TRUE(media->has_audio);
}

TEST_F(TranscoderTest, KeyframesLineUpAcrossRungsAndThePlaylistsFollowTheRules) {
    const auto media = transcode({.size = "1280x720", .rate = "30000/1001", .seconds = 9});
    ASSERT_EQ(ladder_.size(), 2U);
    EXPECT_TRUE(transcoder_.verify(out(), media, ladder_, {}));
    EXPECT_FALSE(progress_.seen.empty());

    const std::string top = ulw::test::keyframe_times(out() / "720p" / "index.m3u8");
    const std::string bottom = ulw::test::keyframe_times(out() / "360p" / "index.m3u8");
    // 0, 4 and 8 s: one keyframe per 4 s segment.
    EXPECT_EQ(std::ranges::count(top, '\n'), 3) << top;
    EXPECT_EQ(top, bottom);

    const std::string playlist = read_text(out() / "360p" / "index.m3u8");
    EXPECT_NE(playlist.find("#EXT-X-VERSION:7"), std::string::npos);
    // Segments of 120 frames at 29.97 fps last 4.004 s; the target rounds to 4.
    EXPECT_NE(playlist.find("#EXT-X-TARGETDURATION:4\n"), std::string::npos);
    EXPECT_NE(playlist.find("#EXT-X-MAP:URI=\"init_1.mp4\""), std::string::npos);
    const std::string master = read_text(out() / "master.m3u8");
    EXPECT_NE(master.find("CODECS=\"avc1.4d4028,mp4a.40.2\""), std::string::npos) << master;
    EXPECT_EQ(entries(out()), (std::vector<std::string>{"360p", "720p", "master.m3u8"}));
}

TEST_F(TranscoderTest, A480pSourceIsNeverUpscaled) {
    const auto media = transcode({.size = "854x480", .rate = "25", .seconds = 3});
    ASSERT_EQ(ladder_.size(), 1U);
    EXPECT_TRUE(transcoder_.verify(out(), media, ladder_, {}));
    EXPECT_EQ(entries(out()), (std::vector<std::string>{"360p", "master.m3u8"}));
    EXPECT_NE(read_text(out() / "master.m3u8").find("RESOLUTION=640x360"), std::string::npos);
}

TEST_F(TranscoderTest, ASilentSourceGetsARungWithoutAudio) {
    const auto media = transcode({.size = "640x360", .rate = "30", .seconds = 3, .audio = false});
    EXPECT_FALSE(media.has_audio);
    EXPECT_TRUE(transcoder_.verify(out(), media, ladder_, {}));
    EXPECT_EQ(read_text(out() / "master.m3u8").find("mp4a"), std::string::npos);
}

TEST_F(TranscoderTest, MisalignedKeyframesFailVerification) {
    const auto media = transcode({.size = "1280x720", .rate = "30000/1001", .seconds = 9});
    // Re-encode the lower rung with a 1.5 s GOP, as a broken command line would.
    fs::remove_all(out() / "360p");
    fs::create_directories(out() / "360p");
    ASSERT_EQ(ulw::test::run_process({"ffmpeg",
                                      "-nostdin",
                                      "-v",
                                      "error",
                                      "-i",
                                      source().string(),
                                      "-vf",
                                      "scale=-2:360",
                                      "-c:v",
                                      "libx264",
                                      "-preset",
                                      "ultrafast",
                                      "-g",
                                      "45",
                                      "-keyint_min",
                                      "45",
                                      "-sc_threshold",
                                      "0",
                                      "-c:a",
                                      "aac",
                                      "-hls_time",
                                      "4",
                                      "-hls_playlist_type",
                                      "vod",
                                      "-hls_segment_type",
                                      "fmp4",
                                      "-hls_fmp4_init_filename",
                                      "init_1.mp4",
                                      "-hls_segment_filename",
                                      (out() / "360p" / "seg_%05d.m4s").string(),
                                      (out() / "360p" / "index.m3u8").string()})
                  .exit_code,
              0);
    const auto verified = transcoder_.verify(out(), media, ladder_, {});
    ASSERT_FALSE(verified);
    EXPECT_EQ(verified.error().kind, TranscodeFailure::Unverified);
    EXPECT_NE(verified.error().detail.find("not aligned"), std::string::npos)
        << verified.error().detail;
}

TEST_F(TranscoderTest, ACorruptSegmentFailsVerification) {
    const auto media = transcode({.size = "640x360", .rate = "30", .seconds = 9});
    const fs::path segment = out() / "360p" / "seg_00001.m4s";
    fs::resize_file(segment, fs::file_size(segment) / 2);
    const auto verified = transcoder_.verify(out(), media, ladder_, {});
    ASSERT_FALSE(verified);
    EXPECT_EQ(verified.error().kind, TranscodeFailure::Unverified) << verified.error().detail;
}

TEST_F(TranscoderTest, AStopRequestEndsTheTranscodePromptly) {
    ASSERT_TRUE(ulw::test::make_clip(
        source(), {.size = "1280x720", .rate = "30", .seconds = 20, .audio = false}));
    const auto media = transcoder_.probe(source(), {});
    ASSERT_TRUE(media);
    const auto ladder = core::choose_ladder(media->height);
    std::stop_source stop;
    std::chrono::steady_clock::time_point stopped_at;
    progress_.on_first = [&] {
        stopped_at = std::chrono::steady_clock::now();
        stop.request_stop();
    };
    const auto stats =
        transcoder_.run(source(), out(), *media, ladder, progress_, stop.get_token());
    ASSERT_FALSE(stats);
    EXPECT_EQ(stats.error().kind, TranscodeFailure::Stopped);
    // ffmpeg honoured SIGTERM: the SIGKILL that follows the grace period was never needed.
    EXPECT_LT(std::chrono::steady_clock::now() - stopped_at, infra::ffmpeg::kTerminationGrace);
}

TEST_F(TranscoderTest, AFileThatIsNotMediaIsRejected) {
    std::ofstream(source()) << "this is not a video";
    const auto media = transcoder_.probe(source(), {});
    ASSERT_FALSE(media);
    EXPECT_EQ(media.error().kind, TranscodeFailure::Rejected);
}

} // namespace
