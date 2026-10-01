// The gateway over the S3 store and a live MinIO, with the store's connections made fewer than the
// uploads it admits, so that uploads wait for one. Production caps the store at its upload slots
// and never waits there (ADR-0045); this holds the gateway to its timers if a store ever does.
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
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

using ulw::test::Backend;
using ulw::test::GatewayOptions;
using ulw::test::GatewayUnderTest;
using ulw::test::HttpClient;
using ulw::test::HttpResponse;
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

    void run(const std::vector<std::size_t>& queue_order);

    ulw::test::LiveS3 target_ = ulw::test::minio_from_env();
    std::vector<std::string> videos_;
};

// The leaders take the store's connections, the rest queue behind them in `queue_order`, a
// body timeout passes, and then every upload finishes.
void GatewayStoreQueue::run(const std::vector<std::size_t>& queue_order) {
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
    for (const std::size_t i : queue_order) {
        ASSERT_TRUE(clients[i]->send_raw(patch_head(uploads[i])));
        ASSERT_TRUE(send(i, 0, kQueuedBytes));
        ingested += kQueuedBytes;
        ASSERT_TRUE(settled());
        // Two turns on, the upload's part has joined libcurl's queue (the turn that read its
        // bytes opened the session, the next ran libcurl), so the next one queues behind it.
        gw.on_loop([] {});
        gw.on_loop([] {});
    }

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

    // The leaders finish and give their connections up, and the queued uploads take them in
    // whatever order libcurl hands them out. It promises none (ADR-0045), and a transfer that
    // loses the race for a freed connection goes to the back of its queue. So every client
    // sends the rest of its body at once, as independent clients would: sent one after the
    // other, the next upload in line may be one the store has not reached, and it would wait
    // for the connections held by uploads whose clients have not sent their rest yet.
    std::vector<std::optional<HttpResponse>> responses(kUploads);
    {
        std::vector<std::jthread> clients_sending;
        clients_sending.reserve(kUploads);
        for (std::size_t i = 0; i < kUploads; ++i) {
            clients_sending.emplace_back([&, i] {
                if (send(i, i < kConnections ? lead : kQueuedBytes, kUploadBytes)) {
                    responses[i] = clients[i]->read_response();
                }
            });
        }
    }
    for (std::size_t i = 0; i < kUploads; ++i) {
        ASSERT_TRUE(responses[i]) << i;
        EXPECT_EQ(responses[i]->status, 204) << i;
        EXPECT_EQ(responses[i]->upload_offset(), kUploadBytes) << i;
    }
}

TEST_F(GatewayStoreQueue, UploadsWaitingForAStoreConnectionAreHeldBackNotFailed) {
    run({2, 3, 4, 5});
}

// libcurl's queue is 5, 4, 3, 2: the first freed connection goes to upload 5, the second to
// upload 2, and the next to upload 4. A test that finished the uploads in their numbered order
// would wait on upload 3, whose turn cannot come while uploads 4 and 5 hold the connections
// waiting for bytes their clients have not sent yet.
TEST_F(GatewayStoreQueue, QueuedUploadsFinishInWhicheverOrderTheStoreTakesThem) {
    run({5, 4, 3, 2});
}

} // namespace
