// live_packager as a separate process, fed by the ffmpeg test publisher over TCP and writing to
// the MinIO of deploy/local/compose.yaml: the M31 packager runs, including the one that dies
// and comes back.
#include "core/models/storage_key.hpp"
#include "infra/storage/s3_transfer.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"
#include "os/unique_fd.hpp"

#include "media_playlist.hpp"
#include "support/child_process.hpp"
#include "support/live_s3.hpp"
#include "support/temp_dir.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

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

std::string env_or(const char* name, const std::string& fallback) {
    // Read while no thread exists that could setenv.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* value = std::getenv(name);
    return value == nullptr || *value == '\0' ? fallback : value;
}

const std::string kAccessKey = env_or("ULW_MINIO_ACCESS_KEY", "ulw-dev");
const std::string kSecretKey = env_or("ULW_MINIO_SECRET_KEY", "ulw-dev-secret");

std::string read_text(const fs::path& file) {
    std::error_code ec;
    std::string text(fs::file_size(file, ec), '\0');
    std::ifstream in(file, std::ios::binary);
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    return text;
}

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

    std::unique_ptr<ChildProcess> start_packager(std::size_t window = 6, unsigned segment = 2) {
        scratch_dirs_.push_back(std::make_unique<TempDir>("ulw-live-scratch"));
        auto packager = ChildProcess::start(
            {ULW_LIVE_PACKAGER_BIN},
            {"PATH=" + env_or("PATH", "/usr/bin:/bin"), "ULW_STREAM_ID=" + stream_,
             "ULW_LIVE_INGEST_PORT=0", "ULW_LIVE_WINDOW_SEGMENTS=" + std::to_string(window),
             "ULW_LIVE_SEGMENT_SECONDS=" + std::to_string(segment), "ULW_STORAGE=minio",
             "ULW_S3_ENDPOINT=" + env_or("ULW_MINIO_ENDPOINT", "http://127.0.0.1:9000"),
             "ULW_BUCKET=" + minio_.bucket, "ULW_S3_ACCESS_KEY_ID=" + kAccessKey,
             "ULW_S3_SECRET_ACCESS_KEY=" + kSecretKey,
             "ULW_SCRATCH_DIR=" + scratch_dirs_.back()->path().string(),
             "ULW_SANDBOX_BIN=" ULW_SANDBOX_BIN});
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

    // A duration of 0 means until killed.
    static std::unique_ptr<ChildProcess> start_publisher(std::uint16_t port, unsigned duration) {
        std::vector<std::string> argv{ULW_LIVE_TESTSOURCE, "127.0.0.1:" + std::to_string(port)};
        if (duration != 0) {
            argv.push_back(std::to_string(duration));
        }
        auto publisher = ChildProcess::start(argv, {"PATH=" + env_or("PATH", "/usr/bin:/bin")});
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
        do {
            sample();
            if (until && until()) {
                return;
            }
        } while (!publisher.wait_exit(kSamplePeriod));
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
            EXPECT_EQ(segment.uri, "seg_" + std::to_string(now->media_sequence + i) + ".m4s");
            EXPECT_TRUE(stored(segment.uri)) << segment.uri << " is listed but not stored";
            EXPECT_TRUE(stored(segment.init));
            seen_.insert(segment.uri);
        }
        last_ = *now;
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
    const auto packager = start_packager(3);
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
    // Segments that slid out of the playlist stay in the store for a lifecycle rule to expire.
    EXPECT_TRUE(stored("seg_0.m4s"));
    // Timestamps for measuring latency, in ascending order a millisecond apart at most from
    // the durations.
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
    EXPECT_LT(packager->output().size(), 2048U) << packager->output();
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
        // The publisher's connection is gone with the packager, and it ends on its own.
        EXPECT_TRUE(publisher->wait_exit(kExitPatience).has_value());
        const auto stored_last = playlist();
        ASSERT_TRUE(stored_last);
        EXPECT_FALSE(stored_last->ended);
        before_next = stored_last->media_sequence + stored_last->segments.size();
    }

    const auto packager = start_packager();
    const auto port = ingest_port(*packager);
    ASSERT_TRUE(port) << packager->output();
    EXPECT_TRUE(packager->wait_for_output("continuing at segment " + std::to_string(before_next),
                                          seconds(10)))
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
        return s.uri == "seg_" + std::to_string(before_next) + ".m4s";
    });
    ASSERT_NE(first_new, final->segments.end());
    EXPECT_TRUE(first_new->discontinuity);
    EXPECT_EQ(first_new->init, "init_1.mp4");
    EXPECT_TRUE(stored("init_1.mp4"));
    EXPECT_EQ(std::ranges::count_if(final->segments,
                                    [](const live::Segment& s) { return s.discontinuity; }),
              1);
}

TEST_F(LivePackagerTest, SigtermEndsTheStreamWithEndlistAndExitsCleanly) {
    const auto packager = start_packager();
    const auto port = ingest_port(*packager);
    ASSERT_TRUE(port) << packager->output();
    const auto publisher = start_publisher(*port, 0);
    watch(*publisher, [&] { return last_ && last_->segments.size() >= 2; });
    ASSERT_FALSE(last_->ended);

    packager->signal(SIGTERM);
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

TEST_F(LivePackagerTest, AnInputThatIsNotMpegtsFailsTheStreamAndPublishesNothing) {
    const auto packager = start_packager();
    const auto port = ingest_port(*packager);
    ASSERT_TRUE(port) << packager->output();
    {
        const os::UniqueFd socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        ASSERT_TRUE(socket);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(*port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API's own cast.
        ASSERT_EQ(
            ::connect(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof address),
            0);
        const std::string garbage(300'000, 'x');
        ASSERT_GT(::write(socket.get(), garbage.data(), garbage.size()), 0);
    }
    EXPECT_EQ(packager->wait_exit(kExitPatience), 1) << packager->output();
    EXPECT_NE(packager->output().find("ffmpeg failed"), std::string::npos) << packager->output();
    EXPECT_FALSE(stored("index.m3u8"));
}

TEST_F(LivePackagerTest, APublisherThatConnectsAndSendsNothingIsGivenUpOn) {
    const auto packager = start_packager();
    const auto port = ingest_port(*packager);
    ASSERT_TRUE(port) << packager->output();
    const os::UniqueFd socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    ASSERT_TRUE(socket);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(*port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API's own cast.
    ASSERT_EQ(::connect(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof address),
              0);
    // Five target durations, and the connection is still open.
    EXPECT_EQ(packager->wait_exit(kExitPatience), 1) << packager->output();
    EXPECT_NE(packager->output().find("no segment for 10 s"), std::string::npos)
        << packager->output();
}

} // namespace
