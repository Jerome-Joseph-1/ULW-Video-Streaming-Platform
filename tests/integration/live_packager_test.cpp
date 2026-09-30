// live_packager as a separate process, fed by the ffmpeg test publisher over SRT and writing to
// the MinIO of deploy/local/compose.yaml: the M31 packager runs, including the one that dies
// and comes back, the one whose store fails, and the two that race for one stream.
#include "core/models/storage_key.hpp"
#include "infra/ffmpeg/live_remux.hpp"
#include "infra/storage/s3_transfer.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"
#include "os/unique_fd.hpp"

#include "fault_proxy.hpp"
#include "media_playlist.hpp"
#include "support/child_process.hpp"
#include "support/live_s3.hpp"
#include "support/srt_caller.hpp"
#include "support/srt_runtime.hpp"
#include "support/temp_dir.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;
using std::chrono::milliseconds;
using std::chrono::seconds;
using ulw::test::ChildProcess;
using ulw::test::TempDir;

constexpr auto kExitPatience = seconds(40);
constexpr auto kSamplePeriod = milliseconds(250);
constexpr std::string_view kPassphrase = "an integration passphrase";

std::string env_or(const char* name, const std::string& fallback) {
    // Read while no thread exists that could setenv.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* value = std::getenv(name);
    return value == nullptr || *value == '\0' ? fallback : value;
}

const std::string kAccessKey = env_or("ULW_MINIO_ACCESS_KEY", "ulw-dev");
const std::string kSecretKey = env_or("ULW_MINIO_SECRET_KEY", "ulw-dev-secret");
const std::string kEndpoint = env_or("ULW_MINIO_ENDPOINT", "http://127.0.0.1:9000");

std::string read_text(const fs::path& file) {
    std::error_code ec;
    std::string text(fs::file_size(file, ec), '\0');
    std::ifstream in(file, std::ios::binary);
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    return text;
}

struct PackagerOptions {
    std::size_t window = 6;
    unsigned segment = 2;
    // The object store the packager talks to; the proxy tests point it at their own.
    std::string endpoint = kEndpoint;
};

class LivePackagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!ulw::test::ensure_bucket(minio_)) {
#ifdef ULW_CONFORMANCE_LIVE
            FAIL() << "MinIO unreachable";
#else
            GTEST_SKIP() << "MinIO unreachable; start deploy/local/compose.yaml";
#endif
        }
        auto store = infra::storage::S3Transfer::create({.credentials = minio_.credentials,
                                                         .clock = clock_,
                                                         .random = random_,
                                                         .profile = minio_.profile,
                                                         .bucket = minio_.bucket});
        ASSERT_TRUE(store);
        store_ = std::move(*store);
        stream_ = "it-" + ulw::test::unique_prefix("s").substr(2, 16);
    }

    void TearDown() override { ulw::test::remove_objects(minio_, "live/" + stream_ + "/"); }

    std::unique_ptr<ChildProcess> start_packager(const PackagerOptions& options = {}) {
        scratch_dirs_.push_back(std::make_unique<TempDir>("ulw-live-scratch"));
        auto packager = ChildProcess::start(
            {ULW_LIVE_PACKAGER_BIN},
            {"PATH=" + env_or("PATH", "/usr/bin:/bin"), "ULW_STREAM_ID=" + stream_,
             "ULW_LIVE_INGEST_PORT=0", "ULW_LIVE_SRT_PASSPHRASE=" + std::string(kPassphrase),
             "ULW_LIVE_WINDOW_SEGMENTS=" + std::to_string(options.window),
             "ULW_LIVE_SEGMENT_SECONDS=" + std::to_string(options.segment), "ULW_STORAGE=minio",
             "ULW_S3_ENDPOINT=" + options.endpoint, "ULW_BUCKET=" + minio_.bucket,
             "ULW_S3_ACCESS_KEY_ID=" + kAccessKey, "ULW_S3_SECRET_ACCESS_KEY=" + kSecretKey,
             "ULW_SCRATCH_DIR=" + scratch_dirs_.back()->path().string(),
             std::string("ULW_SANDBOX_BIN=") + ULW_SANDBOX_BIN});
        EXPECT_NE(packager, nullptr);
        return packager;
    }

    // The port the packager reports it listens on.
    static std::optional<std::uint16_t> ingest_port(ChildProcess& packager) {
        constexpr std::string_view kMark = "ingest=127.0.0.1:";
        if (!packager.wait_for_output(kMark, seconds(20))) {
            return std::nullopt;
        }
        const std::string& out = packager.output();
        const std::size_t at = out.find(kMark) + kMark.size();
        return static_cast<std::uint16_t>(std::stoul(out.substr(at, out.find(' ', at) - at)));
    }

    // A duration of 0 means until killed. `keyframe_seconds` is the publisher's keyframe
    // interval, which is the packager's segment length unless a test says otherwise.
    [[nodiscard]] std::unique_ptr<ChildProcess>
    start_publisher(std::uint16_t port, unsigned duration, unsigned keyframe_seconds = 2) const {
        std::vector<std::string> argv{ULW_LIVE_TESTSOURCE, "127.0.0.1:" + std::to_string(port)};
        if (duration != 0) {
            argv.push_back(std::to_string(duration));
        }
        auto publisher = ChildProcess::start(
            argv, {"PATH=" + env_or("PATH", "/usr/bin:/bin"),
                   "ULW_TESTSOURCE_PASSPHRASE=" + std::string(kPassphrase),
                   "ULW_TESTSOURCE_STREAMID=" + stream_,
                   "ULW_TESTSOURCE_SEGMENT=" + std::to_string(keyframe_seconds)});
        EXPECT_NE(publisher, nullptr);
        return publisher;
    }

    [[nodiscard]] std::optional<live::MediaPlaylist> playlist() {
        const auto key = core::StorageKey::parse("live/" + stream_ + "/index.m3u8");
        const TempDir dir("ulw-live-playlist");
        if (!key || !store_->download(*key, dir.path() / "p.m3u8")) {
            return std::nullopt;
        }
        auto parsed = live::parse_media_playlist(read_text(dir.path() / "p.m3u8"));
        return parsed ? std::optional(std::move(*parsed)) : std::nullopt;
    }

    [[nodiscard]] bool stored(const std::string& name) {
        const auto key = core::StorageKey::parse("live/" + stream_ + "/" + name);
        return key && store_->size(*key).has_value();
    }

    // Reads the playlist every so often, until `until` says stop or the publisher is gone,
    // holding it to the rules of a live playlist at every look: the media sequence never falls,
    // a segment's place is media_sequence + index and its name says so, and everything it lists
    // is in the store.
    void watch(ChildProcess& publisher, const std::function<bool()>& until = {}) {
        while (true) {
            sample();
            if (until && until()) {
                return;
            }
            if (publisher.wait_exit(kSamplePeriod)) {
                break;
            }
        }
        sample();
    }

    void sample() {
        const auto now = playlist();
        if (!now) {
            return;
        }
        EXPECT_GE(now->media_sequence, last_sequence_) << "the media sequence went backwards";
        last_sequence_ = now->media_sequence;
        ++samples_;
        for (std::size_t i = 0; i < now->segments.size(); ++i) {
            const live::Segment& segment = now->segments[i];
            const auto epoch = infra::ffmpeg::live_init_epoch(segment.init);
            ASSERT_TRUE(epoch) << segment.init;
            EXPECT_EQ(segment.uri,
                      infra::ffmpeg::live_segment_name(*epoch, now->media_sequence + i));
            EXPECT_TRUE(stored(segment.uri)) << segment.uri << " is listed but not stored";
            EXPECT_TRUE(stored(segment.init));
            seen_.insert(segment.uri);
        }
        last_ = *now;
    }

    // The bytes in the packager's scratch directory for its ffmpeg output.
    [[nodiscard]] std::uintmax_t scratch_bytes() const {
        std::uintmax_t total = 0;
        std::error_code ec;
        for (const auto& entry : fs::recursive_directory_iterator(
                 scratch_dirs_.back()->path() / stream_ / "media", ec)) {
            if (entry.is_regular_file(ec)) {
                total += entry.file_size(ec);
            }
        }
        return total;
    }

    ulw::test::LiveS3 minio_ = ulw::test::minio_from_env();
    os::SystemClock clock_;
    os::SystemRandom random_;
    std::unique_ptr<infra::storage::S3Transfer> store_;
    std::string stream_;
    std::vector<std::unique_ptr<TempDir>> scratch_dirs_;
    std::uint64_t last_sequence_ = 0;
    std::size_t samples_ = 0;
    std::set<std::string> seen_;
    std::optional<live::MediaPlaylist> last_;
};

TEST_F(LivePackagerTest, ASlidingWindowKeepsAMonotonicSequenceAndEndsWithEndlist) {
    const auto packager = start_packager({.window = 3});
    const auto port = ingest_port(*packager);
    ASSERT_TRUE(port) << packager->output();
    const auto publisher = start_publisher(*port, 24);

    watch(*publisher);
    ASSERT_EQ(packager->wait_exit(kExitPatience), 0) << packager->output();

    const auto final = playlist();
    ASSERT_TRUE(final);
    EXPECT_TRUE(final->ended);
    EXPECT_EQ(final->target_seconds, 2U);
    // 24 s in 2 s segments, and a window of 3: the window slid about nine times.
    EXPECT_GE(final->media_sequence, 6U);
    EXPECT_EQ(final->segments.size(), 3U);
    EXPECT_FALSE(final->segments.front().discontinuity);
    EXPECT_GE(samples_, 20U);
    EXPECT_GE(seen_.size(), 10U);
    EXPECT_TRUE(stored("init_0.mp4"));
    EXPECT_TRUE(stored("epoch_0"));
    // Segments that slid out of the playlist stay in the store for a lifecycle rule to expire.
    EXPECT_TRUE(stored("seg_0_0.m4s"));
    // Wall-clock times that run on by each segment's duration.
    for (std::size_t i = 1; i < final->segments.size(); ++i) {
        ASSERT_TRUE(final->segments[i].program_date_time);
        EXPECT_EQ(*final->segments[i].program_date_time,
                  *final->segments[i - 1].program_date_time +
                      std::chrono::duration_cast<core::WallTime::duration>(
                          final->segments[i - 1].duration));
    }
}

TEST_F(LivePackagerTest, ItsLogHoldsNoSecretAndStaysShort) {
    const auto packager = start_packager();
    const auto port = ingest_port(*packager);
    ASSERT_TRUE(port) << packager->output();
    const auto publisher = start_publisher(*port, 10);
    ASSERT_EQ(publisher->wait_exit(kExitPatience), 0) << publisher->output();
    ASSERT_EQ(packager->wait_exit(kExitPatience), 0) << packager->output();
    EXPECT_EQ(packager->output().find(kSecretKey), std::string::npos);
    EXPECT_EQ(packager->output().find(kAccessKey), std::string::npos);
    EXPECT_EQ(packager->output().find(kPassphrase), std::string::npos);
    EXPECT_LT(packager->output().size(), 2048U) << packager->output();
}

// The publisher joins its encoder mid-interval, so the first video keyframe can reach the
// packager up to a segment length after the audio began. With 2 s segments, video that starts
// 1.8 s after the audio: a 1 s probe window found no picture size in it, ffmpeg could not
// write the init segment, and the stream ended with nothing published.
TEST_F(LivePackagerTest, AStreamWhoseFirstKeyframeComesLateInTheSegmentIsStillPackaged) {
    const std::string path = "PATH=" + env_or("PATH", "/usr/bin:/bin");
    const TempDir dir("ulw-live-late-video");
    const std::string late = (dir.path() / "late.ts").string();
    const auto build = ChildProcess::start({"/usr/bin/env",
                                            "ffmpeg",
                                            "-nostdin",
                                            "-hide_banner",
                                            "-loglevel",
                                            "error",
                                            "-f",
                                            "lavfi",
                                            "-i",
                                            "testsrc2=size=640x360:rate=30",
                                            "-f",
                                            "lavfi",
                                            "-i",
                                            "sine=frequency=440:sample_rate=48000",
                                            "-filter_complex",
                                            "[0:v]setpts=PTS+1.8/TB[v]",
                                            "-map",
                                            "[v]",
                                            "-map",
                                            "1:a",
                                            "-c:v",
                                            "libx264",
                                            "-preset",
                                            "ultrafast",
                                            "-pix_fmt",
                                            "yuv420p",
                                            "-g",
                                            "60",
                                            "-keyint_min",
                                            "60",
                                            "-sc_threshold",
                                            "0",
                                            "-c:a",
                                            "aac",
                                            "-b:a",
                                            "64k",
                                            "-t",
                                            "10",
                                            "-muxdelay",
                                            "0",
                                            "-muxpreload",
                                            "0",
                                            "-f",
                                            "mpegts",
                                            late},
                                           {path});
    ASSERT_NE(build, nullptr);
    ASSERT_EQ(build->wait_exit(kExitPatience), 0) << build->output();

    const auto packager = start_packager();
    const auto port = ingest_port(*packager);
    ASSERT_TRUE(port) << packager->output();
    // Sent in real time as the test publisher sends, the audio's first 1.8 s ahead of any video.
    const auto publisher = ChildProcess::start(
        {"/usr/bin/env",
         "ffmpeg",
         "-nostdin",
         "-hide_banner",
         "-loglevel",
         "warning",
         "-re",
         "-i",
         late,
         "-map",
         "0",
         "-c",
         "copy",
         "-muxdelay",
         "0",
         "-muxpreload",
         "0",
         "-f",
         "mpegts",
         "srt://127.0.0.1:" + std::to_string(*port) + "?mode=caller&pkt_size=1316&passphrase=" +
             std::string(kPassphrase) + "&streamid=" + stream_},
        {path});
    ASSERT_NE(publisher, nullptr);

    watch(*publisher);
    // The packager first: when it gives up, the publisher's own error says only that its
    // connection broke.
    ASSERT_EQ(packager->wait_exit(kExitPatience), 0) << packager->output();
    ASSERT_EQ(publisher->wait_exit(kExitPatience), 0) << publisher->output();
    EXPECT_EQ(packager->output().find("codec parameters"), std::string::npos) << packager->output();
    const auto final = playlist();
    ASSERT_TRUE(final) << packager->output();
    EXPECT_TRUE(final->ended);
    // 8.2 s of video in 2 s segments.
    EXPECT_GE(final->media_sequence + final->segments.size(), 4U) << packager->output();
    EXPECT_TRUE(stored("init_0.mp4"));
}

TEST_F(LivePackagerTest, ACrashedPackagerResumesTheSequenceWhereTheStoreLeftIt) {
    std::uint64_t before_next = 0;
    {
        const auto packager = start_packager();
        const auto port = ingest_port(*packager);
        ASSERT_TRUE(port) << packager->output();
        const auto publisher = start_publisher(*port, 0);
        watch(*publisher, [&] { return last_ && last_->segments.size() >= 4; });
        packager->signal(SIGKILL);
        EXPECT_EQ(packager->wait_exit(kExitPatience), 128 + SIGKILL);
        // SRT notices the packager is gone after its idle timeout, and the publisher ends.
        publisher->signal(SIGKILL);
        const auto stored_last = playlist();
        ASSERT_TRUE(stored_last);
        EXPECT_FALSE(stored_last->ended);
        before_next = stored_last->media_sequence + stored_last->segments.size();
    }

    const auto packager = start_packager();
    const auto port = ingest_port(*packager);
    ASSERT_TRUE(port) << packager->output();
    EXPECT_TRUE(packager->wait_for_output(
        "continuing at segment " + std::to_string(before_next) + " as epoch 1", seconds(10)))
        << packager->output();
    const auto publisher = start_publisher(*port, 12);
    watch(*publisher);
    ASSERT_EQ(packager->wait_exit(kExitPatience), 0) << packager->output();

    const auto final = playlist();
    ASSERT_TRUE(final);
    EXPECT_TRUE(final->ended);
    EXPECT_GE(final->media_sequence + final->segments.size(), before_next + 4);
    // The timeline broke exactly where the new run began, with an init segment of its own.
    const auto first_new = std::ranges::find_if(final->segments, [&](const live::Segment& s) {
        return s.uri == infra::ffmpeg::live_segment_name(1, before_next);
    });
    ASSERT_NE(first_new, final->segments.end());
    EXPECT_TRUE(first_new->discontinuity);
    EXPECT_EQ(first_new->init, "init_1.mp4");
    EXPECT_TRUE(stored("init_1.mp4"));
    EXPECT_EQ(std::ranges::count_if(final->segments,
                                    [](const live::Segment& s) { return s.discontinuity; }),
              1);
}

TEST_F(LivePackagerTest, SigtermDrainsLeavingTheStreamToBeContinued) {
    std::uint64_t before_next = 0;
    {
        const auto packager = start_packager();
        const auto port = ingest_port(*packager);
        ASSERT_TRUE(port) << packager->output();
        const auto publisher = start_publisher(*port, 0);
        watch(*publisher, [&] { return last_ && last_->segments.size() >= 2; });
        ASSERT_FALSE(last_->ended);

        packager->signal(SIGTERM);
        EXPECT_EQ(packager->wait_exit(kExitPatience), 0) << packager->output();
        EXPECT_NE(packager->output().find("drained"), std::string::npos) << packager->output();
        const auto drained = playlist();
        ASSERT_TRUE(drained);
        EXPECT_FALSE(drained->ended);
        EXPECT_GE(drained->segments.size(), 2U);
        before_next = drained->media_sequence + drained->segments.size();
    }
    // The next process for the stream continues it, and a request to end it ends it, even with
    // no publisher there.
    const auto again = start_packager();
    ASSERT_TRUE(ingest_port(*again));
    EXPECT_TRUE(
        again->wait_for_output("continuing at segment " + std::to_string(before_next), seconds(10)))
        << again->output();
    again->signal(SIGUSR1);
    EXPECT_EQ(again->wait_exit(kExitPatience), 0) << again->output();
    const auto ended = playlist();
    ASSERT_TRUE(ended);
    EXPECT_TRUE(ended->ended);
    EXPECT_EQ(ended->media_sequence + ended->segments.size(), before_next);
}

TEST_F(LivePackagerTest, SigusrEndsTheStreamWithEndlistAndExitsCleanly) {
    const auto packager = start_packager();
    const auto port = ingest_port(*packager);
    ASSERT_TRUE(port) << packager->output();
    const auto publisher = start_publisher(*port, 0);
    watch(*publisher, [&] { return last_ && last_->segments.size() >= 2; });

    packager->signal(SIGUSR1);
    EXPECT_EQ(packager->wait_exit(kExitPatience), 0) << packager->output();
    const auto final = playlist();
    ASSERT_TRUE(final);
    EXPECT_TRUE(final->ended);
    EXPECT_GE(final->segments.size(), 2U);
    EXPECT_NE(packager->output().find("stream ended"), std::string::npos);
}

TEST_F(LivePackagerTest, SigtermBeforeAnyPublisherLeavesNothingToEnd) {
    const auto packager = start_packager();
    ASSERT_TRUE(ingest_port(*packager));
    packager->signal(SIGTERM);
    EXPECT_EQ(packager->wait_exit(kExitPatience), 0) << packager->output();
    EXPECT_FALSE(stored("index.m3u8"));
}

TEST_F(LivePackagerTest, AnEndedStreamIsNotStartedAgain) {
    {
        const auto packager = start_packager();
        const auto port = ingest_port(*packager);
        ASSERT_TRUE(port) << packager->output();
        const auto publisher = start_publisher(*port, 5);
        ASSERT_EQ(packager->wait_exit(kExitPatience), 0) << packager->output();
    }
    const auto again = start_packager();
    EXPECT_EQ(again->wait_exit(kExitPatience), 1) << again->output();
    EXPECT_NE(again->output().find("stream already ended"), std::string::npos) << again->output();
    EXPECT_TRUE(playlist()->ended);
}

TEST_F(LivePackagerTest, ARestartWithAnotherSegmentLengthIsRefused) {
    {
        const auto packager = start_packager();
        const auto port = ingest_port(*packager);
        ASSERT_TRUE(port) << packager->output();
        const auto publisher = start_publisher(*port, 0);
        watch(*publisher, [&] { return last_ && last_->segments.size() >= 2; });
        packager->signal(SIGTERM);
        EXPECT_EQ(packager->wait_exit(kExitPatience), 0) << packager->output();
    }
    const auto again = start_packager({.segment = 4});
    EXPECT_EQ(again->wait_exit(kExitPatience), 1) << again->output();
    EXPECT_NE(again->output().find("another segment length"), std::string::npos) << again->output();
}

TEST_F(LivePackagerTest,
       CallersWithAWrongPassphraseOrStreamIdAreRefusedAndThePublisherStillGetsIn) {
    const auto packager = start_packager();
    const auto port = ingest_port(*packager);
    ASSERT_TRUE(port) << packager->output();
    {
        const ulw::test::SrtCaller wrong_passphrase;
        EXPECT_FALSE(wrong_passphrase.connect(*port, "not the right passphrase", stream_));
        const ulw::test::SrtCaller wrong_stream;
        EXPECT_FALSE(wrong_stream.connect(*port, kPassphrase, "another-stream"));
        const ulw::test::SrtCaller unencrypted;
        EXPECT_FALSE(unencrypted.connect(*port, "", stream_));
    }
    const auto publisher = start_publisher(*port, 6);
    ASSERT_EQ(packager->wait_exit(kExitPatience), 0) << packager->output();
    const auto final = playlist();
    ASSERT_TRUE(final);
    EXPECT_TRUE(final->ended);
    EXPECT_GE(final->segments.size(), 2U);
}

TEST_F(LivePackagerTest, AnAuthenticatedInputThatIsNotMpegtsIsGivenUpOnAndPublishesNothing) {
    const auto packager = start_packager();
    const auto port = ingest_port(*packager);
    ASSERT_TRUE(port) << packager->output();
    {
        const ulw::test::SrtCaller caller;
        ASSERT_TRUE(caller.connect(*port, kPassphrase, stream_));
        const std::string garbage(1316, 'x');
        for (int i = 0; i < 300; ++i) {
            ASSERT_TRUE(caller.send(garbage));
        }
        // ffmpeg looks for a sync byte in bytes that never have one, and finishes no segment.
        EXPECT_EQ(packager->wait_exit(kExitPatience), 1) << packager->output();
    }
    EXPECT_NE(packager->output().find("no segment finished for 10 s"), std::string::npos)
        << packager->output();
    EXPECT_FALSE(stored("index.m3u8"));
}

TEST_F(LivePackagerTest, APublisherThatConnectsAndSendsNothingIsGivenUpOn) {
    const auto packager = start_packager();
    const auto port = ingest_port(*packager);
    ASSERT_TRUE(port) << packager->output();
    const ulw::test::SrtCaller caller;
    ASSERT_TRUE(caller.connect(*port, kPassphrase, stream_));
    // Five target durations, and the connection is still open.
    EXPECT_EQ(packager->wait_exit(kExitPatience), 1) << packager->output();
    EXPECT_NE(packager->output().find("the publisher sent nothing for 10 s"), std::string::npos)
        << packager->output();
}

TEST_F(LivePackagerTest, APublisherThatNeverSendsAKeyframeStallsTheStreamWithinTheDiskBound) {
    const auto packager = start_packager();
    const auto port = ingest_port(*packager);
    ASSERT_TRUE(port) << packager->output();
    // One keyframe at the start and none for the next 1000 s: the first segment never ends.
    const auto publisher = start_publisher(*port, 0, 1000);
    std::uintmax_t peak = 0;
    while (!packager->wait_exit(milliseconds(500))) {
        peak = std::max(peak, scratch_bytes());
    }
    EXPECT_EQ(packager->wait_exit(seconds(1)), 1) << packager->output();
    EXPECT_NE(packager->output().find("no segment finished for 10 s"), std::string::npos)
        << packager->output();
    // The scratch directory held one growing segment, under the limit ffmpeg was given.
    EXPECT_LE(peak, infra::ffmpeg::live_max_file_bytes(20'000, 2));
    EXPECT_FALSE(stored("seg_0_0.m4s"));
}

// A store that fails every upload for a while, between the packager and MinIO.

TEST_F(LivePackagerTest, AStoreOutageShorterThanThePatienceDoesNotEndTheStream) {
    ulw::test::FaultProxy proxy(kEndpoint);
    const auto packager = start_packager({.window = 10, .endpoint = proxy.endpoint()});
    const auto port = ingest_port(*packager);
    ASSERT_TRUE(port) << packager->output();
    const auto publisher = start_publisher(*port, 50);
    watch(*publisher, [&] { return last_ && last_->segments.size() >= 3; });

    // 13 s of refused uploads: past the 10 s that ffmpeg may go without finishing a segment,
    // inside the 20 s a window's worth of failures is borne.
    proxy.fail_puts(true);
    EXPECT_FALSE(publisher->wait_exit(seconds(13)));
    proxy.fail_puts(false);
    watch(*publisher);
    ASSERT_EQ(packager->wait_exit(kExitPatience), 0) << packager->output();

    EXPECT_EQ(packager->output().find("giving up"), std::string::npos) << packager->output();
    EXPECT_EQ(packager->output().find("no segment finished"), std::string::npos)
        << packager->output();
    const auto final = playlist();
    ASSERT_TRUE(final);
    EXPECT_TRUE(final->ended);
    // Every segment ffmpeg cut of the 50 s, the ones of the outage included, was published:
    // 25 of them, less at most one for the partial one at either end.
    EXPECT_GE(final->media_sequence + final->segments.size(), 24U);
}

TEST_F(LivePackagerTest, AStoreOutageLongerThanThePatienceEndsTheStreamAndNamesTheStore) {
    ulw::test::FaultProxy proxy(kEndpoint);
    // A window of 3 segments is borne 6 s of failures.
    const auto packager = start_packager({.window = 3, .endpoint = proxy.endpoint()});
    const auto port = ingest_port(*packager);
    ASSERT_TRUE(port) << packager->output();
    const auto publisher = start_publisher(*port, 0);
    watch(*publisher, [&] { return last_ && last_->segments.size() >= 2; });

    proxy.fail_puts(true);
    EXPECT_EQ(packager->wait_exit(kExitPatience), 1) << packager->output();
    EXPECT_NE(packager->output().find("giving up after 6 s of failed uploads"), std::string::npos)
        << packager->output();
    EXPECT_EQ(packager->output().find("no segment finished"), std::string::npos)
        << packager->output();
}

// Two packagers for one stream: the later one is the writer.

TEST_F(LivePackagerTest, ASecondPackagerOnTheStreamSupersedesTheFirstWithoutOverwritingItsOutput) {
    const auto older = start_packager();
    const auto older_port = ingest_port(*older);
    ASSERT_TRUE(older_port) << older->output();
    const auto older_publisher = start_publisher(*older_port, 0);
    watch(*older_publisher, [&] { return last_ && last_->segments.size() >= 3; });

    const auto newer = start_packager();
    const auto newer_port = ingest_port(*newer);
    ASSERT_TRUE(newer_port) << newer->output();
    EXPECT_TRUE(newer->wait_for_output("as epoch 1", seconds(10))) << newer->output();
    // The older one finds the claim at its next playlist write, at most a segment later, and
    // stops without ending the stream.
    EXPECT_EQ(older->wait_exit(kExitPatience), 1) << older->output();
    EXPECT_NE(older->output().find("newer packager"), std::string::npos) << older->output();
    older_publisher->signal(SIGKILL);
    const auto after_older = playlist();
    ASSERT_TRUE(after_older);
    EXPECT_FALSE(after_older->ended);

    const auto newer_publisher = start_publisher(*newer_port, 10);
    watch(*newer_publisher);
    ASSERT_EQ(newer->wait_exit(kExitPatience), 0) << newer->output();
    const auto final = playlist();
    ASSERT_TRUE(final);
    EXPECT_TRUE(final->ended);
    // The stream goes on from what the older one had published, in the newer one's epoch.
    EXPECT_TRUE(std::ranges::any_of(final->segments,
                                    [](const live::Segment& s) { return s.init == "init_1.mp4"; }));
    EXPECT_GE(final->media_sequence + final->segments.size(), last_sequence_);
}

} // namespace
