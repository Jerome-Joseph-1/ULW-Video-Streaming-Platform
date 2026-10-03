// gateway_server as a separate process over a scratch Postgres database and MinIO: the store
// connections its uploads get are the ones its own startup gives the S3 multi, which no in-process
// harness goes through.
#include "core/util/json.hpp"
#include "core/util/parse.hpp"
#include "infra/curl/multi.hpp"
#include "os/system_clock.hpp"

#include "devtoken/dev_key.hpp"
#include "postgres_harness.hpp"
#include "support/child_process.hpp"
#include "support/http_client.hpp"
#include "support/live_s3.hpp"
#include "support/reserve_port.hpp"
#include "support/temp_dir.hpp"

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using std::chrono::milliseconds;
using std::chrono::seconds;
using ulw::test::ChildProcess;
using ulw::test::HttpClient;
using ulw::test::ScratchDatabase;

constexpr std::string_view kIssuer = "ulw-store-connections-test";
constexpr std::size_t kKiB = 1024;
// One more upload than a multi left at its default connection count holds.
constexpr std::size_t kUploads = infra::curl::Multi::kDefaultMaxConnections + 1;
// One part each, which holds a store connection from its first byte to its last.
constexpr std::size_t kUploadBytes = 1024 * kKiB;
// More than the gateway takes in for an upload whose part has no store connection: the session's
// 64 KiB buffer and at most 256 KiB staged. All of it arriving proves the part has one.
constexpr std::size_t kLeadBytes = 512 * kKiB;

std::string env_or(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value == nullptr || *value == '\0' ? fallback : value;
}

class GatewayStoreConnections : public ::testing::Test {
protected:
    void SetUp() override {
        if (!ulw::test::ensure_bucket(minio_)) {
#ifdef ULW_CONFORMANCE_LIVE
            FAIL() << "MinIO unreachable";
#else
            GTEST_SKIP() << "MinIO unreachable; start deploy/local/compose.yaml";
#endif
        }
        ScratchDatabase::open(db_);
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        start_gateway();
    }

    void TearDown() override {
        if (gateway_) {
            gateway_->signal(SIGTERM);
            EXPECT_EQ(gateway_->wait_exit(seconds(30)), 0) << gateway_->output();
        }
        for (const std::string& video : videos_) {
            ulw::test::abort_uploads(minio_, "videos/" + video + "/");
        }
    }

    void start_gateway() {
        auto key = devtoken::DevKey::generate();
        ASSERT_TRUE(key);
        const fs::path jwks = files_.path() / "jwks.json";
        std::ofstream(jwks) << key->public_jwks();
        token_ = *key->mint({.issuer = std::string(kIssuer),
                             .audience = "ulw-dev",
                             .subject = "alice",
                             .email = {},
                             .ttl = seconds(600)},
                            clock_.wall_now());
        std::vector<std::string> env{
            "ULW_STORAGE=minio",
            "ULW_S3_ENDPOINT=" + env_or("ULW_MINIO_ENDPOINT", "http://127.0.0.1:9000"),
            "ULW_BUCKET=" + minio_.bucket,
            "ULW_S3_ACCESS_KEY_ID=" + env_or("ULW_MINIO_ACCESS_KEY", "ulw-dev"),
            "ULW_S3_SECRET_ACCESS_KEY=" + env_or("ULW_MINIO_SECRET_KEY", "ulw-dev-secret"),
            "ULW_DATABASE_URL=" + db_->conninfo(), "ULW_DEV_JWKS_FILE=" + jwks.string(),
            "ULW_DEV_MODE=1", "JWT_ISSUER=" + std::string(kIssuer),
            // One user holds every upload here.
            "ULW_MAX_UPLOADS_PER_USER=" + std::to_string(kUploads),
            // Some runs start tests as root; this suite is not about that.
            "ULW_ALLOW_ROOT=1"};
        if (const char* reactor = std::getenv("ULW_REACTOR")) {
            env.push_back("ULW_REACTOR=" + std::string(reactor));
        }
        auto started = ulw::test::start_until_listening(
            [&] {
                port_ = ulw::test::reserve_port();
                auto with_port = env;
                with_port.push_back("ULW_LISTEN_PORT=" + std::to_string(port_));
                return port_ == 0 ? nullptr : ChildProcess::start({ULW_GATEWAY_BIN}, with_port);
            },
            R"("event":"listening")", seconds(30));
        gateway_ = std::move(started.process);
        ASSERT_NE(gateway_, nullptr);
        ASSERT_TRUE(started.ready) << gateway_->output();
    }

    // The upload's id; its video is remembered so that its store upload can be aborted after.
    [[nodiscard]] std::string create(HttpClient& c) {
        const std::string body = R"({"filename":"trip.mp4","size_bytes":)" +
                                 std::to_string(kUploadBytes) + R"(,"content_type":"video/mp4"})";
        const auto r = c.request("POST", "/api/v1/uploads", token_, std::as_bytes(std::span(body)));
        if (!r || r->status != 201) {
            return {};
        }
        const auto doc = core::json::parse(r->body);
        if (!doc) {
            return {};
        }
        videos_.emplace_back(*doc->find("video_id")->as_string());
        return std::string(*doc->find("upload_id")->as_string());
    }

    // bytes_ingested_total, or nullopt when /metrics could not be read.
    [[nodiscard]] std::optional<std::uint64_t> ingested() const {
        HttpClient c({.port = port_});
        const auto r = c.request("GET", "/metrics", "");
        constexpr std::string_view kName = "\nbytes_ingested_total ";
        const std::size_t at = r ? r->body.find(kName) : std::string::npos;
        if (at == std::string::npos) {
            return std::nullopt;
        }
        const std::string_view rest = std::string_view(r->body).substr(at + kName.size());
        return core::parse_integer<std::uint64_t>(rest.substr(0, rest.find('\n')));
    }

    os::SystemClock clock_;
    ulw::test::LiveS3 minio_ = ulw::test::minio_from_env();
    std::unique_ptr<ScratchDatabase> db_;
    ulw::test::TempDir files_{"ulw-store-connections"};
    std::unique_ptr<ChildProcess> gateway_;
    std::uint16_t port_ = 0;
    std::string token_;
    std::vector<std::string> videos_;
};

// Every upload the gateway admits streams its part to the store at once (ADR-0045). A multi with
// fewer connections than admitted uploads queues the rest inside libcurl, where no timer runs:
// their clients stall, taking nothing, until a part ahead of them ends.
TEST_F(GatewayStoreConnections, MoreUploadsThanADefaultMultiHoldsEachHaveAStoreConnection) {
    const std::vector<std::byte> data(kLeadBytes, std::byte{'x'});
    std::vector<std::unique_ptr<HttpClient>> clients;
    for (std::size_t i = 0; i < kUploads; ++i) {
        clients.push_back(std::make_unique<HttpClient>(ulw::test::Endpoint{.port = port_}));
        const std::string upload = create(*clients.back());
        ASSERT_FALSE(upload.empty());
        ASSERT_TRUE(clients.back()->send_raw(
            "PATCH /api/v1/uploads/" + upload + " HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer " +
            token_ + "\r\nUpload-Offset: 0\r\nContent-Length: " + std::to_string(kUploadBytes) +
            "\r\n\r\n"));
        ASSERT_TRUE(clients.back()->send_raw(std::span(data)));
    }
    std::optional<std::uint64_t> seen;
    EXPECT_TRUE(gateway_->poll_until(
        [&] {
            seen = ingested();
            return seen == kUploads * kLeadBytes;
        },
        seconds(10), milliseconds(250)))
        << "ingested " << seen.value_or(0) << " of " << kUploads * kLeadBytes;
}

} // namespace
