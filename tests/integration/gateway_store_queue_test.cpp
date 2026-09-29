// The gateway over the S3 store and a live MinIO, with the store's connections made few, so that
// uploads queue behind them as they queue behind the 64 of production under load.
#include "core/util/json.hpp"

#include "gateway_harness.hpp"
#include "support/eventually.hpp"
#include "support/http_client.hpp"
#include "support/live_s3.hpp"
#include "support/reactor_harness.hpp"

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <poll.h>
#include <span>
#include <string>
#include <vector>

namespace {

using ulw::test::Backend;
using ulw::test::GatewayOptions;
using ulw::test::GatewayUnderTest;
using ulw::test::HttpClient;
using ulw::test::kKiB;
using ulw::test::kMiB;

constexpr std::string_view kToken = "user.alice";
constexpr std::size_t kConnections = 2;
constexpr std::size_t kUploads = 6;
// One part each, which holds a store connection from its first byte to its last.
constexpr std::size_t kUploadBytes = kMiB;
// More than the gateway can take in for an upload whose part is not being sent: the session's
// 64 KiB buffer plus at most 256 KiB staged. All of it arriving proves the part has a connection.
constexpr std::size_t kLeadBytes = 512 * kKiB;
// Fills a queued part's 64 KiB buffer and leaves the rest staged, so the gateway stops reading.
constexpr std::size_t kQueuedBytes = 96 * kKiB;

std::string patch_head(const std::string& upload) {
    return "PATCH /api/v1/uploads/" + upload + " HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer " +
           std::string(kToken) +
           "\r\nUpload-Offset: 0\r\nContent-Length: " + std::to_string(kUploadBytes) + "\r\n\r\n";
}

bool answered(const HttpClient& c) {
    pollfd p{.fd = c.fd(), .events = POLLIN, .revents = 0};
    return ::poll(&p, 1, 0) == 1;
}

class GatewayStoreQueue : public ::testing::Test {
protected:
    void SetUp() override {
        if (!ulw::test::ensure_bucket(target_)) {
#ifdef ULW_CONFORMANCE_LIVE
            FAIL() << "MinIO unreachable";
#else
            GTEST_SKIP() << "MinIO unreachable; start deploy/local/compose.yaml or set "
                            "ULW_MINIO_ENDPOINT";
#endif
        }
    }
    void TearDown() override {
        for (const std::string& video : videos_) {
            ulw::test::abort_uploads(target_, "videos/" + video + "/");
        }
    }

    // The upload's id; its video is remembered so the store's upload can be aborted after.
    [[nodiscard]] std::string create(HttpClient& c) {
        const std::string body = R"({"filename":"trip.mp4","size_bytes":)" +
                                 std::to_string(kUploadBytes) + R"(,"content_type":"video/mp4"})";
        const auto r = c.request("POST", "/api/v1/uploads", kToken, std::as_bytes(std::span(body)));
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

    ulw::test::LiveS3 target_ = ulw::test::minio_from_env();
    std::vector<std::string> videos_;
};

TEST_F(GatewayStoreQueue, UploadsWaitingForAStoreConnectionAreHeldBackNotFailed) {
    GatewayOptions options{
        .backend = Backend::S3, .store_connections = kConnections, .manual_clock = true};
    options.limits.max_uploads_per_user = kUploads;
    const gateway::Limits& limits = options.limits;
    GatewayUnderTest gw(options);
    const auto data = ulw::test::pattern(kUploadBytes);

    std::vector<std::unique_ptr<HttpClient>> clients;
    std::vector<std::string> uploads;
    for (std::size_t i = 0; i < kUploads; ++i) {
        clients.push_back(std::make_unique<HttpClient>(gw.endpoint()));
        uploads.push_back(create(*clients.back()));
        ASSERT_FALSE(uploads.back().empty());
    }
    const auto send = [&](std::size_t i, std::size_t from, std::size_t to) {
        return clients[i]->send_raw(std::span(data).subspan(from, to - from));
    };
    std::uint64_t ingested = 0;
    const auto settled = [&] {
        return ulw::test::eventually([&] { return gw.counters().bytes_ingested == ingested; });
    };

    for (std::size_t i = 0; i < kConnections; ++i) {
        ASSERT_TRUE(clients[i]->send_raw(patch_head(uploads[i])));
        ASSERT_TRUE(send(i, 0, kLeadBytes));
        ingested += kLeadBytes;
    }
    ASSERT_TRUE(settled());
    for (std::size_t i = kConnections; i < kUploads; ++i) {
        ASSERT_TRUE(clients[i]->send_raw(patch_head(uploads[i])));
        ASSERT_TRUE(send(i, 0, kQueuedBytes));
        ingested += kQueuedBytes;
    }
    ASSERT_TRUE(settled());

    // A body timeout passes with the queued uploads held up by the store, while the uploads
    // holding its connections keep above the minimum rate: 100 KiB per 10 s against 80 KiB.
    constexpr int kSteps = 3;
    const core::Millis step = limits.body_idle_timeout / kSteps;
    constexpr std::size_t kStepBytes = 100 * kKiB;
    static_assert(kLeadBytes + (kSteps * kStepBytes) < kUploadBytes, "the leaders must not finish");
    std::size_t lead = kLeadBytes;
    for (int s = 0; s < kSteps; ++s) {
        gw.advance(step);
        for (std::size_t i = 0; i < kConnections; ++i) {
            ASSERT_TRUE(send(i, lead, lead + kStepBytes));
            ingested += kStepBytes;
        }
        lead += kStepBytes;
        ASSERT_TRUE(settled());
    }
    for (std::size_t i = kConnections; i < kUploads; ++i) {
        EXPECT_FALSE(answered(*clients[i])) << "upload " << i << " answered while queued";
    }
    const gateway::Counters mid = gw.counters();
    EXPECT_EQ(mid.timeouts_body + mid.timeouts_body_rate, 0U);

    // The leaders finish and give their connections up; the queued uploads take them in turn.
    for (std::size_t i = 0; i < kUploads; ++i) {
        ASSERT_TRUE(send(i, i < kConnections ? lead : kQueuedBytes, kUploadBytes)) << i;
        const auto r = clients[i]->read_response();
        ASSERT_TRUE(r) << i;
        EXPECT_EQ(r->status, 204) << i;
        EXPECT_EQ(r->upload_offset(), kUploadBytes) << i;
    }
}

} // namespace
