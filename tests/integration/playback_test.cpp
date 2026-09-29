// gateway_server as a separate process in front of a scratch Postgres database and MinIO,
// serving what the worker's ffmpeg command writes. A viewer's playlists come from the gateway;
// every segment comes from the object store, through URLs only the gateway could have signed.
#include "core/models/ids.hpp"
#include "core/models/ladder.hpp"
#include "infra/curl/http.hpp"
#include "infra/storage/s3_transfer.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "command.hpp"
#include "devtoken/dev_key.hpp"
#include "media_clips.hpp"
#include "postgres_harness.hpp"
#include "support/child_process.hpp"
#include "support/free_port.hpp"
#include "support/live_s3.hpp"
#include "support/temp_dir.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using infra::postgres::Params;
using std::chrono::seconds;
using ulw::test::ChildProcess;
using ulw::test::ScratchDatabase;
using ulw::test::TempDir;

constexpr std::string_view kIssuer = "ulw-playback-test";

std::string env_or(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value == nullptr || *value == '\0' ? fallback : value;
}

std::string read_text(const fs::path& p) {
    const std::ifstream in(p, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::vector<std::string> lines(std::string_view text) {
    std::vector<std::string> out;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        out.emplace_back(text.substr(0, nl));
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
    }
    return out;
}

infra::curl::Result get(const std::string& url, const std::string& token = {}) {
    infra::curl::Request request{.method = infra::curl::Method::Get,
                                 .url = url,
                                 .headers = {},
                                 // Far above any segment of a few seconds at 360 lines.
                                 .max_body = std::size_t{16} << 20U,
                                 .timeout = std::chrono::milliseconds(10'000)};
    if (!token.empty()) {
        request.headers.push_back("Authorization: Bearer " + token);
    }
    return infra::curl::perform(request);
}

// The URL a playlist line names: the line itself, or the URI attribute of an EXT-X-MAP.
std::optional<std::string> uri_of(const std::string& line) {
    constexpr std::string_view kMap = "#EXT-X-MAP:URI=\"";
    if (line.starts_with(kMap)) {
        return line.substr(kMap.size(), line.size() - kMap.size() - 1);
    }
    if (!line.empty() && !line.starts_with('#')) {
        return line;
    }
    return std::nullopt;
}

class PlaybackTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!ulw::test::ensure_bucket(minio_)) {
#ifdef ULW_CONFORMANCE_LIVE
            FAIL() << "MinIO unreachable";
#else
            GTEST_SKIP() << "MinIO unreachable; start deploy/local/compose.yaml";
#endif
        }
        if (ulw::test::run_process({"ffmpeg", "-version"}).exit_code != 0) {
            GTEST_SKIP() << "ffmpeg is not installed";
        }
        ScratchDatabase::open(db_);
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        publish_video();
        start_gateway();
    }

    void TearDown() override {
        if (gateway_) {
            gateway_->signal(SIGTERM);
            EXPECT_EQ(gateway_->wait_exit(seconds(30)), 0) << gateway_->output();
        }
        if (published_) {
            ulw::test::remove_objects(minio_, "videos/" + video_.to_string() + "/");
        }
    }

    // What the worker leaves behind: its ffmpeg command's output under the video's prefix, and
    // the video ready.
    void publish_video() {
        const fs::path clip = files_.path() / "clip.mp4";
        ASSERT_TRUE(ulw::test::make_clip(
            clip, {.size = "640x360", .rate = "30", .seconds = 6, .audio = true}));
        const std::vector<core::Rung> ladder{{.name = "360p", .height = 360, .video_kbps = 800},
                                             {.name = "240p", .height = 240, .video_kbps = 400}};
        out_ = files_.path() / "out";
        for (const core::Rung& r : ladder) {
            fs::create_directories(out_ / r.name);
        }
        const core::ports::MediaInfo media{.width = 640,
                                           .height = 360,
                                           .frame_rate = {.num = 30, .den = 1},
                                           .duration = core::Millis{6'000},
                                           .has_audio = true};
        ASSERT_EQ(ulw::test::run_process(
                      infra::ffmpeg::transcode_args("ffmpeg", clip, out_, media, ladder, 1))
                      .exit_code,
                  0);

        auto store = infra::storage::S3Transfer::create({.credentials = minio_.credentials,
                                                         .clock = clock_,
                                                         .random = random_,
                                                         .profile = minio_.profile,
                                                         .bucket = minio_.bucket});
        ASSERT_TRUE(store);
        const auto type = *core::ContentType::parse("application/octet-stream");
        published_ = true;
        for (const auto& entry : fs::recursive_directory_iterator(out_)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::string rel = fs::relative(entry.path(), out_).generic_string();
            ASSERT_TRUE(
                (*store)->upload(entry.path(), *core::StorageKey::parse(prefix() + rel), type))
                << rel;
        }
        auto conn = db_->session();
        ASSERT_TRUE(
            conn.exec("INSERT INTO videos (id, owner_id, title, state, version, duration_ms) "
                      "VALUES ($1, 'alice', 'clip', 'ready', 3, 6000)",
                      Params{}.add_uuid(video_.uuid())));
    }

    void start_gateway() {
        auto key = devtoken::DevKey::generate();
        ASSERT_TRUE(key);
        const fs::path jwks = files_.path() / "jwks.json";
        std::ofstream(jwks) << key->public_jwks();
        const auto mint = [&](std::string subject) {
            return *key->mint({.issuer = std::string(kIssuer),
                               .audience = "askedin-platform",
                               .subject = std::move(subject),
                               .email = {},
                               .ttl = seconds(600)},
                              clock_.wall_now());
        };
        alice_ = mint("alice");
        bob_ = mint("bob");

        port_ = ulw::test::free_port();
        ASSERT_NE(port_, 0);
        std::vector<std::string> env{
            "ULW_LISTEN_PORT=" + std::to_string(port_),
            "ULW_STORAGE=minio",
            "ULW_S3_ENDPOINT=" + env_or("ULW_MINIO_ENDPOINT", "http://127.0.0.1:9000"),
            "ULW_BUCKET=" + minio_.bucket,
            "ULW_S3_ACCESS_KEY_ID=" + env_or("ULW_MINIO_ACCESS_KEY", "ulw-dev"),
            "ULW_S3_SECRET_ACCESS_KEY=" + env_or("ULW_MINIO_SECRET_KEY", "ulw-dev-secret"),
            "ULW_DATABASE_URL=" + db_->conninfo(),
            "ULW_DEV_JWKS_FILE=" + jwks.string(),
            "JWT_ISSUER=" + std::string(kIssuer)};
        // Every request here is a connection of its own from 127.0.0.1, several a second.
        env.emplace_back("ULW_NEW_CONNECTIONS_PER_IP_PER_SECOND=1000");
        if (const char* reactor = std::getenv("ULW_REACTOR")) {
            env.push_back("ULW_REACTOR=" + std::string(reactor));
        }
        gateway_ = ChildProcess::start({ULW_GATEWAY_BIN}, env);
        ASSERT_NE(gateway_, nullptr);
        ASSERT_TRUE(gateway_->wait_for_output(R"("port":)" + std::to_string(port_), seconds(30)))
            << gateway_->output();
    }

    [[nodiscard]] std::string prefix() const { return "videos/" + video_.to_string() + "/hls/"; }
    [[nodiscard]] std::string api(std::string_view rest) const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/api/v1/videos/" +
               video_.to_string() + "/" + std::string(rest);
    }

    os::SystemClock clock_;
    os::SystemRandom random_;
    ulw::test::LiveS3 minio_ = ulw::test::minio_from_env();
    core::VideoId video_ = core::VideoId::generate(clock_, random_);
    std::unique_ptr<ScratchDatabase> db_;
    TempDir files_{"ulw-playback"};
    fs::path out_;
    std::string alice_;
    std::string bob_;
    std::uint16_t port_ = 0;
    std::unique_ptr<ChildProcess> gateway_;
    bool published_ = false;
};

TEST_F(PlaybackTest, SegmentsComeFromTheStoreByteForByteAndNeverFromTheGateway) {
    const auto master = get(api("master.m3u8"), alice_);
    ASSERT_TRUE(master && master->status == 200) << (master ? master->body : master.error().detail);
    std::size_t gateway_bytes = master->body.size();
    std::size_t store_bytes = 0;
    std::size_t fetched = 0;
    const std::string store_base = env_or("ULW_MINIO_ENDPOINT", "http://127.0.0.1:9000") + "/" +
                                   minio_.bucket + "/" + prefix();

    for (const std::string rung : {"360p", "240p"}) {
        const auto media = get(api(rung + "/index.m3u8"), alice_);
        ASSERT_TRUE(media && media->status == 200);
        EXPECT_EQ(media->header("cache-control"), "private, max-age=60");
        gateway_bytes += media->body.size();
        for (const std::string& line : lines(media->body)) {
            const auto url = uri_of(line);
            if (!url) {
                continue;
            }
            ASSERT_TRUE(url->starts_with(store_base + rung + "/")) << *url;
            // A 6 s video gets the one-hour floor.
            EXPECT_NE(url->find("X-Amz-Expires=3600&"), std::string::npos) << *url;
            const std::string name =
                url->substr(store_base.size(), url->find('?') - store_base.size());
            const auto body = get(*url);
            ASSERT_TRUE(body) << body.error().detail;
            ASSERT_EQ(body->status, 200) << *url << "\n" << body->body;
            EXPECT_EQ(body->body, read_text(out_ / name)) << name;
            store_bytes += body->body.size();
            ++fetched;
        }
    }
    // Two rungs of an init segment and two media segments each, at the least.
    EXPECT_GE(fetched, 6U);
    // Everything the gateway sent was playlists, a sliver of what the store served.
    EXPECT_LT(gateway_bytes * 20, store_bytes);
    // And it has no route that could serve a segment.
    const auto direct = get(api("360p/seg_00000.m4s"), alice_);
    ASSERT_TRUE(direct);
    EXPECT_EQ(direct->status, 404);
}

TEST_F(PlaybackTest, TheBucketItselfRefusesUnsignedAndTamperedReads) {
    const auto media = get(api("360p/index.m3u8"), alice_);
    ASSERT_TRUE(media && media->status == 200);
    std::optional<std::string> url;
    for (const std::string& line : lines(media->body)) {
        if (!line.starts_with('#')) {
            url = line;
            break;
        }
    }
    ASSERT_TRUE(url);
    ASSERT_EQ(get(*url)->status, 200);
    EXPECT_EQ(get(url->substr(0, url->find('?')))->status, 403);
    std::string tampered = *url;
    tampered.back() = tampered.back() == '0' ? '1' : '0';
    EXPECT_EQ(get(tampered)->status, 403);
}

TEST_F(PlaybackTest, SomeoneElseGetsNothingToSign) {
    EXPECT_EQ(get(api("master.m3u8"), bob_)->status, 404);
    EXPECT_EQ(get(api("360p/index.m3u8"), bob_)->status, 404);
    EXPECT_EQ(get(api("360p/index.m3u8"))->status, 401);
}

TEST_F(PlaybackTest, AMasterFetchIsWrittenAsAViewByTheTimeTheGatewayHasDrained) {
    ASSERT_EQ(get(api("master.m3u8"), alice_)->status, 200);
    ASSERT_EQ(get(api("360p/index.m3u8"), alice_)->status, 200);
    // The batch is due 5 s after the view; a drain writes it at once.
    gateway_->signal(SIGTERM);
    ASSERT_EQ(gateway_->wait_exit(seconds(30)), 0) << gateway_->output();
    gateway_.reset();
    auto conn = db_->session();
    EXPECT_EQ(ulw::test::scalar(conn,
                                "SELECT concat_ws(' ', count(*), min(viewer_id)) FROM view_events "
                                "WHERE video_id = $1",
                                Params{}.add_uuid(video_.uuid())),
              "1 alice");
}

} // namespace
