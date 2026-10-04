// The adapter against the real ffmpeg and ffprobe, in the real sandbox, on generated clips.
#include "core/models/ladder.hpp"
#include "infra/ffmpeg/transcoder.hpp"
#include "os/system_clock.hpp"

#include "exit_code.hpp"
#include "media_clips.hpp"
#include "process.hpp"
#include "support/temp_dir.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <iterator>
#include <map>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
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

// Every file under `dir`, by its path relative to it, with its bytes.
std::map<std::string, std::string> tree(const fs::path& dir) {
    std::map<std::string, std::string> files;
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (e.is_regular_file()) {
            std::ifstream in(e.path(), std::ios::binary);
            std::string bytes(e.file_size(), '\0');
            in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            files[fs::relative(e.path(), dir).string()] = std::move(bytes);
        }
    }
    return files;
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
    infra::ffmpeg::FfmpegTranscoder transcoder_{
        {.sandbox = ULW_SANDBOX_BIN, .search_path = search_path(), .threads = 2}, clock_};
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

TEST_F(TranscoderTest, FfmpegOutOfCpuTimeIsOverBudgetNotKilled) {
    // ffmpeg catches SIGXCPU and starts a graceful exit, which the hard limit's SIGKILL cuts
    // short; its exit status alone would read as a kill by somebody else.
    const auto child = infra::ffmpeg::run_sandboxed(
        {.helper = ULW_SANDBOX_BIN, .environment = {"PATH=" + search_path()}},
        {.writable = work_.path(),
         .address_space_bytes = std::uint64_t{4} << 30U,
         .cpu = core::Seconds{1},
         .wall = core::Millis{60'000}},
        {"ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i",
         "testsrc2=size=1920x1080:rate=30", "-t", "600", "-c:v", "libx264", "-preset", "veryslow",
         "-f", "null", "-"},
        clock_, [](std::string_view) {}, {});
    ASSERT_TRUE(child) << child.error();
    EXPECT_NE(child->exit_code, 0);
    EXPECT_EQ(child->ending, infra::ffmpeg::Ending::CpuExhausted) << child->exit_code;
    EXPECT_EQ(infra::ffmpeg::classify(child->exit_code, child->signal, child->ending),
              TranscodeFailure::OverBudget);
}

TEST_F(TranscoderTest, FfmpegsOwnFailuresAbove128RejectTheInput) {
    // ffmpeg exits with 256 minus its error code, here 256 - ENOENT; read as 128 + a signal
    // it would count as a kill and requeue a bad file until its attempts ran out.
    const auto child = infra::ffmpeg::run_sandboxed(
        {.helper = ULW_SANDBOX_BIN, .environment = {"PATH=" + search_path()}},
        {.writable = work_.path(),
         .address_space_bytes = std::uint64_t{4} << 30U,
         .cpu = core::Seconds{60},
         .wall = core::Millis{60'000}},
        {"ffmpeg", "-nostdin", "-v", "error", "-i", (work_.path() / "absent.mp4").string(), "-f",
         "null", "-"},
        clock_, [](std::string_view) {}, {});
    ASSERT_TRUE(child) << child.error();
    EXPECT_GT(child->exit_code, 128);
    EXPECT_EQ(child->signal, 0);
    EXPECT_EQ(infra::ffmpeg::classify(child->exit_code, child->signal, child->ending),
              TranscodeFailure::Rejected);
}

TEST_F(TranscoderTest, AManifestThatNamesAFileOutsideTheWorkspaceIsRefused) {
    // An upload is only ever one file; a DASH manifest makes ffmpeg read whatever it names.
    const ulw::test::TempDir elsewhere("ulw-elsewhere");
    const fs::path secret = elsewhere.path() / "secret.mp4";
    ASSERT_TRUE(ulw::test::make_clip(
        secret, {.size = "640x360", .rate = "30", .seconds = 2, .audio = false}));
    // Named as the worker names every upload, with nothing to hint at the format.
    const fs::path upload = work_.path() / "source";
    std::ofstream(upload)
        << "<?xml version=\"1.0\"?>\n"
           "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"static\" "
           "mediaPresentationDuration=\"PT2S\" minBufferTime=\"PT1S\" "
           "profiles=\"urn:mpeg:dash:profile:isoff-on-demand:2011\"><Period>"
           "<AdaptationSet mimeType=\"video/mp4\"><Representation id=\"1\" bandwidth=\"100000\" "
           "width=\"640\" height=\"360\" codecs=\"avc1.64001e\"><BaseURL>"
        << secret.string()
        << "</BaseURL><SegmentBase/></Representation></AdaptationSet></Period></MPD>\n";
    // Without the whitelist, ffprobe follows the manifest and describes the file it names.
    ASSERT_EQ(ulw::test::run_process({"ffprobe", "-v", "error", upload.string()}).exit_code, 0);

    const auto media = transcoder_.probe(upload, {});
    ASSERT_FALSE(media);
    EXPECT_EQ(media.error().kind, TranscodeFailure::Rejected);

    const MediaInfo claimed{.width = 640,
                            .height = 360,
                            .frame_rate = {.num = 30, .den = 1},
                            .duration = core::Millis{2000},
                            .has_audio = false};
    const auto ladder = core::choose_ladder(claimed.height);
    const auto stats = transcoder_.run(upload, out(), claimed, ladder, progress_, {});
    ASSERT_FALSE(stats);
    EXPECT_FALSE(fs::exists(out() / "master.m3u8"));
}

TEST_F(TranscoderTest, SegmentsOfTwoRunsMixIntoARenditionThatPlays) {
    // A rerun, or a zombie's late upload, overwrites some keys of another run's output; x264
    // with a VBV and frame threads writes different bytes each time, so this is what readers
    // may get.
    const auto media = transcode({.size = "1280x720", .rate = "30000/1001", .seconds = 9});
    const fs::path again = work_.path() / "again";
    const auto stats = transcoder_.run(source(), again, media, ladder_, progress_, {});
    ASSERT_TRUE(stats) << stats.error().detail;
    const auto first = tree(out());
    const auto second = tree(again);
    ASSERT_EQ(first.size(), second.size());
    for (const auto& [name, bytes] : first) {
        ASSERT_TRUE(second.contains(name)) << name;
        if (!name.ends_with(".m4s")) {
            EXPECT_EQ(second.at(name), bytes) << name;
        }
    }
    // Take every other segment from the second run.
    for (const auto& [name, bytes] : second) {
        if (name.ends_with("1.m4s")) {
            fs::copy_file(again / name, out() / name, fs::copy_options::overwrite_existing);
        }
    }
    const auto verified = transcoder_.verify(out(), media, ladder_, {});
    EXPECT_TRUE(verified) << verified.error().detail;
}

TEST_F(TranscoderTest, AFileThatIsNotMediaIsRejected) {
    std::ofstream(source()) << "this is not a video";
    const auto media = transcoder_.probe(source(), {});
    ASSERT_FALSE(media);
    EXPECT_EQ(media.error().kind, TranscodeFailure::Rejected);
}

// The soak's self-hosted runs: a source the sandboxed ffprobe may not open (there, root without
// capabilities behind a home directory of mode 750; here, a file of mode 000, which neither the
// owner nor a capability-less root may read) is the host's fault, not the upload's.
TEST_F(TranscoderTest, ASourceTheSandboxMayNotReadIsInaccessibleNotRejected) {
    ASSERT_TRUE(ulw::test::make_clip(source(), {.size = "320x240", .rate = "25", .seconds = 1}));
    fs::permissions(source(), fs::perms::none);
    const auto media = transcoder_.probe(source(), {});
    ASSERT_FALSE(media);
    EXPECT_EQ(media.error().kind, TranscodeFailure::Inaccessible) << media.error().detail;
    EXPECT_NE(media.error().detail.find("Permission denied"), std::string::npos)
        << media.error().detail;

    const MediaInfo claimed{.width = 320,
                            .height = 240,
                            .frame_rate = {.num = 25, .den = 1},
                            .duration = core::Millis{1000},
                            .has_audio = false};
    const auto stats =
        transcoder_.run(source(), out(), claimed, core::choose_ladder(240), progress_, {});
    ASSERT_FALSE(stats);
    EXPECT_EQ(stats.error().kind, TranscodeFailure::Inaccessible) << stats.error().detail;
    fs::permissions(source(), fs::perms::owner_read | fs::perms::owner_write);
}

TEST_F(TranscoderTest, AnOutputDirectoryTheSandboxMayNotWriteIsInaccessible) {
    ASSERT_TRUE(ulw::test::make_clip(source(), {.size = "320x240", .rate = "25", .seconds = 1}));
    const auto media = transcoder_.probe(source(), {});
    ASSERT_TRUE(media) << media.error().detail;
    fs::create_directories(out());
    fs::permissions(out(), fs::perms::owner_read | fs::perms::owner_exec);
    const auto stats =
        transcoder_.run(source(), out(), *media, core::choose_ladder(media->height), progress_, {});
    fs::permissions(out(), fs::perms::owner_all);
    ASSERT_FALSE(stats);
    EXPECT_EQ(stats.error().kind, TranscodeFailure::Inaccessible) << stats.error().detail;
}

} // namespace
