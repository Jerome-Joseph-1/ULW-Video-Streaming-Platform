// What the worker's own ffmpeg command writes, not a hand-written sample: a few seconds of
// lavfi test pattern through transcode_args, then its segments checked the way a player's
// buffer sees them, and both playlists through the rewriter.
#include "core/models/ladder.hpp"
#include "core/ports/transcoder.hpp"
#include "core/util/hls.hpp"

#include "command.hpp"
#include "support/temp_dir.hpp"

#include <sys/wait.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <set>
#include <spawn.h>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;

// posix_spawnp's search, with the output discarded; the exit code, or -1.
int run(std::vector<std::string> argv) {
    std::vector<char*> ptrs;
    ptrs.reserve(argv.size() + 1);
    for (std::string& a : argv) {
        ptrs.push_back(a.data());
    }
    ptrs.push_back(nullptr);
    posix_spawn_file_actions_t actions{};
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    const int spawned = ::posix_spawnp(&pid, ptrs[0], &actions, nullptr, ptrs.data(), ::environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) {
        return -1;
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

std::string slurp(const fs::path& p) {
    const std::ifstream in(p, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// A trailing newline ends the last line rather than starting an empty one.
std::vector<std::string> lines(std::string_view text) {
    std::vector<std::string> out;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        out.emplace_back(text.substr(0, nl));
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
    }
    return out;
}

// ffprobe's "12.345678", or 0 for "N/A".
double seconds(std::string_view text) {
    double value = 0;
    // from_chars takes a [first, last) pointer pair.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

// The end of each track in a segment, in seconds: the latest pts + duration of its packets.
std::map<std::string, double> track_ends(const fs::path& init, const fs::path& segment,
                                         const fs::path& scratch) {
    const fs::path joined = scratch / "joined.mp4";
    const fs::path listing = scratch / "packets.csv";
    {
        std::ofstream out(joined, std::ios::binary);
        out << slurp(init) << slurp(segment);
    }
    if (run({"ffprobe", "-v", "error", "-show_entries", "packet=codec_type,pts_time,duration_time",
             "-of", "csv=p=0", "-o", listing.string(), joined.string()}) != 0) {
        return {};
    }
    std::map<std::string, double> ends;
    for (const std::string& line : lines(slurp(listing))) {
        const std::size_t a = line.find(',');
        const std::size_t b = line.find(',', a + 1);
        if (a == std::string::npos || b == std::string::npos) {
            continue;
        }
        const std::string_view text = line;
        double& end = ends[line.substr(0, a)];
        end = std::max(end, seconds(text.substr(a + 1, b - a - 1)) + seconds(text.substr(b + 1)));
    }
    return ends;
}

class WorkerHlsOutput : public ::testing::Test {
protected:
    // Two rungs are enough to have a master with more than one variant, and 360 lines keep
    // the encode to a second or two.
    const std::vector<core::Rung> ladder_{{.name = "360p", .height = 360, .video_kbps = 800},
                                          {.name = "240p", .height = 240, .video_kbps = 400}};

    void SetUp() override {
        if (run({"ffmpeg", "-version"}) != 0) {
            GTEST_SKIP() << "ffmpeg is not installed";
        }
        const fs::path clip = dir_.path() / "clip.mp4";
        // Nine seconds at 4 s segments: two full segments and a short last one.
        ASSERT_EQ(run({"ffmpeg",
                       "-nostdin",
                       "-v",
                       "error",
                       "-y",
                       "-f",
                       "lavfi",
                       "-i",
                       "testsrc2=size=640x360:rate=30",
                       "-f",
                       "lavfi",
                       "-i",
                       "sine=frequency=440:sample_rate=48000",
                       "-t",
                       "9",
                       "-c:v",
                       "libx264",
                       "-preset",
                       "ultrafast",
                       "-pix_fmt",
                       "yuv420p",
                       "-c:a",
                       "aac",
                       "-shortest",
                       clip.string()}),
                  0);
        const core::ports::MediaInfo media{.width = 640,
                                           .height = 360,
                                           .frame_rate = {.num = 30, .den = 1},
                                           .duration = core::Millis{9000},
                                           .has_audio = true};
        out_ = dir_.path() / "out";
        for (const core::Rung& r : ladder_) {
            fs::create_directories(out_ / r.name);
        }
        ASSERT_EQ(run(infra::ffmpeg::transcode_args("ffmpeg", clip, out_, media, ladder_, 1)), 0);
    }

    ulw::test::TempDir dir_{"ulw-hls"};
    fs::path out_;
};

// A player appends each fMP4 segment, audio and video together, into one buffer, and that
// buffer only covers the time both tracks do. Audio stopping short of the video at a segment's
// end leaves every segment partly unbuffered, which hls.js reports as an append without
// progress whenever a rung switch appends a segment over buffered media.
TEST_F(WorkerHlsOutput, EverySegmentsAudioReachesTheEndOfItsVideo) {
    // Audio may overrun by up to one AAC frame, 1024 samples at 48 kHz; a millisecond of slack
    // covers the rounding of ffprobe's six decimals.
    constexpr double kAacFrame = 1024.0 / 48'000;
    constexpr double kSlack = 0.001;
    for (const core::Rung& rung : ladder_) {
        std::vector<fs::path> segments;
        fs::path init;
        for (const auto& entry : fs::directory_iterator(out_ / rung.name)) {
            if (entry.path().extension() == ".m4s") {
                segments.push_back(entry.path());
            } else if (entry.path().extension() == ".mp4") {
                init = entry.path();
            }
        }
        std::ranges::sort(segments);
        ASSERT_GE(segments.size(), 3U) << rung.name;
        for (const fs::path& segment : segments) {
            const auto ends = track_ends(init, segment, dir_.path());
            ASSERT_TRUE(ends.contains("audio") && ends.contains("video")) << segment;
            const double audio = ends.at("audio");
            const double video = ends.at("video");
            EXPECT_GE(audio, video - kSlack) << segment << ": audio ends before its video";
            if (segment != segments.back()) {
                EXPECT_LE(audio, video + kAacFrame + kSlack) << segment;
            }
        }
    }
}

TEST_F(WorkerHlsOutput, MasterListsEveryRungAndRoutesItThroughTheGateway) {
    const std::string master = slurp(out_ / "master.m3u8");
    const auto names = core::hls::list_renditions(master);
    ASSERT_TRUE(names) << core::hls::to_string(names.error()) << "\n" << master;
    EXPECT_EQ(*names, (std::vector<std::string_view>{"360p", "240p"}));

    const auto out = core::hls::rewrite_master(master, "/api/v1/videos/v/");
    ASSERT_TRUE(out) << core::hls::to_string(out.error());
    std::vector<std::string> uris;
    std::size_t stream_tags = 0;
    for (const std::string& line : lines(*out)) {
        ASSERT_FALSE(line.empty());
        if (line.starts_with("#EXT-X-STREAM-INF:")) {
            ++stream_tags;
            // CODECS and BANDWIDTH are what hls.js picks a rung by; they pass untouched.
            EXPECT_NE(master.find(line), std::string::npos) << line;
        } else if (!line.starts_with('#')) {
            uris.push_back(line);
        }
    }
    EXPECT_EQ(stream_tags, 2U);
    EXPECT_EQ(uris, (std::vector<std::string>{"/api/v1/videos/v/360p/index.m3u8",
                                              "/api/v1/videos/v/240p/index.m3u8"}));
}

TEST_F(WorkerHlsOutput, MediaPlaylistSignsTheInitAndEverySegmentTheWorkerPublishes) {
    for (const core::Rung& rung : ladder_) {
        const std::string media = slurp(out_ / rung.name / "index.m3u8");
        const std::string dir = "videos/v/hls/" + rung.name + "/";
        std::set<std::string> signed_names;
        const auto out = core::hls::rewrite_media(
            media, dir, [&](const core::StorageKey& key) -> std::optional<std::string> {
                signed_names.insert(std::string(key.view().substr(dir.size())));
                return "http://127.0.0.1:9000/b/" + key.str() + "?X-Amz-Signature=0";
            });
        ASSERT_TRUE(out) << core::hls::to_string(out.error()) << "\n" << media;

        // Exactly the files the worker would upload for this rung: the init segment and every
        // .m4s, nothing more and nothing left out.
        std::set<std::string> on_disk;
        for (const auto& entry : fs::directory_iterator(out_ / rung.name)) {
            if (entry.path().filename() != "index.m3u8") {
                on_disk.insert(entry.path().filename().string());
            }
        }
        EXPECT_EQ(signed_names, on_disk) << rung.name;
        EXPECT_TRUE(std::ranges::any_of(on_disk, [](const std::string& n) {
            return n.ends_with(".mp4");
        })) << rung.name;

        const std::vector<std::string> rewritten = lines(*out);
        std::size_t extinf = 0;
        for (std::size_t i = 0; i < rewritten.size(); ++i) {
            const std::string& line = rewritten[i];
            ASSERT_FALSE(line.empty()) << "blank line " << i;
            if (line.starts_with("#EXTINF:")) {
                ++extinf;
                ASSERT_LT(i + 1, rewritten.size());
                EXPECT_TRUE(rewritten[i + 1].starts_with("http://127.0.0.1:9000/b/" + dir))
                    << rewritten[i + 1];
            }
            if (line.starts_with("#EXT-X-MAP:")) {
                EXPECT_TRUE(line.starts_with("#EXT-X-MAP:URI=\"http://127.0.0.1:9000/b/" + dir))
                    << line;
            } else if (line.starts_with('#')) {
                EXPECT_NE(media.find(line + "\n"), std::string::npos) << line;
            }
        }
        EXPECT_EQ(extinf, on_disk.size() - 1) << rung.name;
        EXPECT_TRUE(out->ends_with("#EXT-X-ENDLIST\n"));
    }
}

} // namespace
