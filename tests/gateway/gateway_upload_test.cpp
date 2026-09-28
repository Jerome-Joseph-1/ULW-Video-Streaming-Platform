#include "core/util/json.hpp"

#include "gateway_harness.hpp"
#include "support/eventually.hpp"
#include "support/http_client.hpp"
#include "support/reactor_harness.hpp"

#include <array>
#include <gtest/gtest.h>
#include <iterator>
#include <openssl/evp.h>
#include <thread>
#include <vector>

namespace {

using ulw::test::Backend;
using ulw::test::GatewayOptions;
using ulw::test::GatewayUnderTest;
using ulw::test::HttpClient;
using ulw::test::HttpResponse;
using ulw::test::kMiB;

constexpr std::string_view kAlice = "user:alice";
constexpr std::string_view kBob = "user:bob";

std::string sha256(std::span<const std::byte> bytes) {
    std::array<unsigned char, 32> md{};
    std::size_t len = md.size();
    EVP_Q_digest(nullptr, "SHA256", nullptr, bytes.data(), bytes.size(), md.data(), &len);
    std::string hex;
    for (const unsigned char c : md) {
        constexpr std::string_view kHex = "0123456789abcdef";
        hex += kHex[c >> 4U];
        hex += kHex[c & 0xFU];
    }
    return hex;
}

struct Created {
    std::string video_id;
    std::string upload_id;
    std::uint64_t chunk_size = 0;
};

std::optional<Created> create_upload(HttpClient& c, std::uint64_t size,
                                     std::string_view token = kAlice) {
    const std::string body = R"({"filename":"trip.mp4","size_bytes":)" + std::to_string(size) +
                             R"(,"content_type":"video/mp4"})";
    const auto r = c.request("POST", "/api/v1/uploads", token, std::as_bytes(std::span(body)));
    if (!r || r->status != 201) {
        return std::nullopt;
    }
    const auto doc = core::json::parse(r->body);
    if (!doc) {
        return std::nullopt;
    }
    return Created{.video_id = std::string(*doc->find("video_id")->as_string()),
                   .upload_id = std::string(*doc->find("upload_id")->as_string()),
                   .chunk_size = *doc->find("chunk_size")->as_u64()};
}

std::optional<HttpResponse> patch(HttpClient& c, const std::string& upload, std::uint64_t offset,
                                  std::span<const std::byte> bytes,
                                  std::string_view token = kAlice) {
    return c.request("PATCH", "/api/v1/uploads/" + upload, token, bytes,
                     {{"Upload-Offset", std::to_string(offset)}});
}

// Sends every chunk in order, following the server's Upload-Offset.
bool upload_all(HttpClient& c, const Created& up, std::span<const std::byte> data) {
    std::uint64_t offset = 0;
    while (offset < data.size()) {
        const std::size_t n = std::min<std::uint64_t>(up.chunk_size, data.size() - offset);
        const auto r = patch(c, up.upload_id, offset, data.subspan(offset, n));
        if (!r || r->status != 204 || !r->upload_offset()) {
            return false;
        }
        offset = *r->upload_offset();
    }
    return true;
}

TEST(GatewayUpload, HealthAndReadinessNeedNoToken) {
    const GatewayUnderTest gw({});
    HttpClient c(gw.port());
    EXPECT_EQ(c.request("GET", "/api/v1/healthz", "")->status, 200);
    EXPECT_EQ(c.request("GET", "/api/v1/readyz", "")->status, 200);
    EXPECT_EQ(c.request("GET", "/api/v1/uploads", "")->status, 405);
}

TEST(GatewayUpload, UploadRoutesRefuseMissingAndBadTokens) {
    const GatewayUnderTest gw({});
    HttpClient c(gw.port());
    const std::string body = R"({"filename":"a.mp4","size_bytes":10,"content_type":"video/mp4"})";
    EXPECT_EQ(c.request("POST", "/api/v1/uploads", "", std::as_bytes(std::span(body)))->status,
              401);
    HttpClient d(gw.port());
    EXPECT_EQ(
        d.request("POST", "/api/v1/uploads", "forged", std::as_bytes(std::span(body)))->status,
        401);
}

TEST(GatewayUpload, InboundUserHeadersAreIgnored) {
    const GatewayUnderTest gw({});
    HttpClient c(gw.port());
    const std::string body = R"({"filename":"a.mp4","size_bytes":10,"content_type":"video/mp4"})";
    const auto r = c.request("POST", "/api/v1/uploads", "", std::as_bytes(std::span(body)),
                             {{"x-user-id", "alice"}, {"x-user-email", "a@example.com"}});
    EXPECT_EQ(r->status, 401);
}

TEST(GatewayUpload, HundredMegabytesReassembleByteIdentical) {
    GatewayUnderTest gw({.backend = Backend::Fs});
    const auto data = ulw::test::pattern(100 * kMiB, 11);
    HttpClient c(gw.port());
    const auto up = create_upload(c, data.size());
    ASSERT_TRUE(up);
    EXPECT_EQ(up->chunk_size, 8 * kMiB);
    ASSERT_TRUE(upload_all(c, *up, data));

    const auto head = c.request("HEAD", "/api/v1/uploads/" + up->upload_id, kAlice);
    ASSERT_TRUE(head);
    EXPECT_EQ(head->status, 204);
    EXPECT_EQ(head->upload_offset(), data.size());

    const auto commit = c.request("POST", "/api/v1/uploads/" + up->upload_id + "/commit", kAlice);
    ASSERT_TRUE(commit);
    EXPECT_EQ(commit->status, 200);
    const auto stored = gw.reader().fetch_small(
        *core::StorageKey::parse("videos/" + up->video_id + "/raw"), data.size());
    ASSERT_TRUE(stored);
    EXPECT_EQ(sha256(*stored), sha256(data));

    const auto video = c.request("GET", "/api/v1/videos/" + up->video_id, kAlice);
    ASSERT_TRUE(video);
    EXPECT_EQ(video->status, 200);
    EXPECT_EQ(core::json::parse(video->body)->find("state")->as_string(), "processing");
}

TEST(GatewayUpload, CommitIsIdempotentAndQueuesOneJob) {
    GatewayUnderTest gw({.backend = Backend::Fake, .chunk = kMiB});
    const auto data = ulw::test::pattern((3 * kMiB) + 17);
    HttpClient c(gw.port());
    const auto up = create_upload(c, data.size());
    ASSERT_TRUE(up);
    ASSERT_TRUE(upload_all(c, *up, data));
    const std::string commit = "/api/v1/uploads/" + up->upload_id + "/commit";
    EXPECT_EQ(c.request("POST", commit, kAlice)->status, 200);
    EXPECT_EQ(c.request("POST", commit, kAlice)->status, 200);
    const auto jobs = gw.jobs();
    ASSERT_EQ(jobs.size(), 1U);
    EXPECT_EQ(jobs[0].source_key.str(), "videos/" + up->video_id + "/raw");
}

TEST(GatewayUpload, CommitBeforeEveryByteArrivedIs409) {
    GatewayUnderTest gw({.backend = Backend::Fake, .chunk = kMiB});
    const auto data = ulw::test::pattern(2 * kMiB);
    HttpClient c(gw.port());
    const auto up = create_upload(c, data.size());
    ASSERT_TRUE(up);
    ASSERT_EQ(patch(c, up->upload_id, 0, std::span(data).first(kMiB))->status, 204);
    const auto r = c.request("POST", "/api/v1/uploads/" + up->upload_id + "/commit", kAlice);
    EXPECT_EQ(r->status, 409);
    EXPECT_TRUE(gw.jobs().empty());
}

TEST(GatewayUpload, ClientKilledMidChunkResumesFromHead) {
    GatewayUnderTest gw({.backend = Backend::Fs, .chunk = kMiB});
    const auto data = ulw::test::pattern((5 * kMiB) + 1234, 5);
    std::string upload;
    std::string video;
    {
        HttpClient c(gw.port());
        const auto up = create_upload(c, data.size());
        ASSERT_TRUE(up);
        upload = up->upload_id;
        video = up->video_id;
        ASSERT_EQ(patch(c, upload, 0, std::span(data).first(2 * kMiB))->status, 204);
        // Declare a whole chunk, send half of it, and vanish.
        const std::string head = "PATCH /api/v1/uploads/" + upload +
                                 " HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer user:alice\r\n"
                                 "Upload-Offset: " +
                                 std::to_string(2 * kMiB) +
                                 "\r\nContent-Length: " + std::to_string(kMiB) + "\r\n\r\n";
        ASSERT_TRUE(c.send_raw(head));
        ASSERT_TRUE(c.send_raw(std::span(data).subspan(2 * kMiB, kMiB / 2)));
    }
    // The claim is released once the server notices the disconnect; HEAD then reports
    // what is durable, which excludes the half chunk.
    ASSERT_TRUE(ulw::test::eventually([&] { return gw.claims() == 0; }));
    HttpClient c(gw.port());
    std::uint64_t offset = 0;
    const auto head = c.request("HEAD", "/api/v1/uploads/" + upload, kAlice);
    ASSERT_TRUE(head && head->upload_offset());
    offset = *head->upload_offset();
    EXPECT_EQ(offset, 2 * kMiB);
    while (offset < data.size()) {
        const std::size_t n = std::min<std::uint64_t>(kMiB, data.size() - offset);
        const auto r = patch(c, upload, offset, std::span(data).subspan(offset, n));
        ASSERT_TRUE(r);
        ASSERT_EQ(r->status, 204);
        offset = *r->upload_offset();
    }
    ASSERT_EQ(c.request("POST", "/api/v1/uploads/" + upload + "/commit", kAlice)->status, 200);
    const auto stored =
        gw.reader().fetch_small(*core::StorageKey::parse("videos/" + video + "/raw"), data.size());
    ASSERT_TRUE(stored);
    EXPECT_EQ(sha256(*stored), sha256(data));
}

TEST(GatewayUpload, OffsetMismatchIs409WithTheAuthoritativeOffset) {
    const GatewayUnderTest gw({.backend = Backend::Fake, .chunk = kMiB});
    const auto data = ulw::test::pattern(3 * kMiB);
    HttpClient c(gw.port());
    const auto up = create_upload(c, data.size());
    ASSERT_TRUE(up);
    ASSERT_EQ(patch(c, up->upload_id, 0, std::span(data).first(kMiB))->status, 204);
    HttpClient d(gw.port());
    const auto r = patch(d, up->upload_id, 2 * kMiB, std::span(data).subspan(2 * kMiB, kMiB));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 409);
    EXPECT_EQ(r->upload_offset(), kMiB);
}

TEST(GatewayUpload, AnotherUsersUploadIsIndistinguishableFromAMissingOne) {
    const GatewayUnderTest gw({.backend = Backend::Fake, .chunk = kMiB});
    HttpClient c(gw.port());
    const auto up = create_upload(c, kMiB);
    ASSERT_TRUE(up);
    const auto data = ulw::test::pattern(kMiB);
    HttpClient b1(gw.port());
    EXPECT_EQ(patch(b1, up->upload_id, 0, data, kBob)->status, 404);
    HttpClient b2(gw.port());
    EXPECT_EQ(b2.request("HEAD", "/api/v1/uploads/" + up->upload_id, kBob)->status, 404);
    EXPECT_EQ(b2.request("GET", "/api/v1/videos/" + up->video_id, kBob)->status, 404);
    EXPECT_EQ(b2.request("POST", "/api/v1/uploads/" + up->upload_id + "/commit", kBob)->status,
              404);
    EXPECT_EQ(
        b2.request("GET", "/api/v1/videos/00000000-0000-7000-8000-000000000000", kBob)->status,
        404);
}

TEST(GatewayUpload, MalformedIdsAreRefusedNotRepaired) {
    const GatewayUnderTest gw({.backend = Backend::Fake});
    HttpClient c(gw.port());
    for (const std::string_view id :
         {"0192F3C4-7A1B-7C2D-8E3F-0123456789AB", "..", "abc", "%2e%2e"}) {
        const auto r = c.request("HEAD", "/api/v1/uploads/" + std::string(id), kAlice);
        ASSERT_TRUE(r) << id;
        EXPECT_EQ(r->status, 404) << id;
    }
}

TEST(GatewayUpload, ConcurrentAppendToOneUploadIs409) {
    GatewayUnderTest gw({.backend = Backend::Fake, .chunk = kMiB});
    const auto data = ulw::test::pattern(2 * kMiB);
    HttpClient c(gw.port());
    const auto up = create_upload(c, data.size());
    ASSERT_TRUE(up);
    HttpClient first(gw.port());
    const std::string head = "PATCH /api/v1/uploads/" + up->upload_id +
                             " HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer user:alice\r\n"
                             "Upload-Offset: 0\r\nContent-Length: " +
                             std::to_string(kMiB) + "\r\n\r\n";
    ASSERT_TRUE(first.send_raw(head));
    ASSERT_TRUE(first.send_raw(std::span(data).first(1000)));
    ASSERT_TRUE(ulw::test::eventually([&] { return gw.claims() == 1; }));

    HttpClient second(gw.port());
    const auto r = patch(second, up->upload_id, 0, std::span(data).first(kMiB));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 409);
    EXPECT_EQ(r->upload_offset(), 0U);

    ASSERT_TRUE(first.send_raw(std::span(data).subspan(1000, kMiB - 1000)));
    const auto done = first.read_response();
    ASSERT_TRUE(done);
    EXPECT_EQ(done->status, 204);
    EXPECT_EQ(done->upload_offset(), kMiB);
}

TEST(GatewayUpload, AdmissionCapsConcurrentUploadsPerUser) {
    GatewayUnderTest gw({.backend = Backend::Fake, .chunk = kMiB});
    const auto data = ulw::test::pattern(kMiB);
    std::vector<std::unique_ptr<HttpClient>> holders;
    for (int i = 0; i < 3; ++i) {
        HttpClient setup(gw.port());
        const auto up = create_upload(setup, kMiB);
        ASSERT_TRUE(up);
        auto h = std::make_unique<HttpClient>(gw.port());
        const std::string head = "PATCH /api/v1/uploads/" + up->upload_id +
                                 " HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer user:alice\r\n"
                                 "Upload-Offset: 0\r\nContent-Length: " +
                                 std::to_string(kMiB) + "\r\n\r\n";
        ASSERT_TRUE(h->send_raw(head));
        holders.push_back(std::move(h));
    }
    ASSERT_TRUE(ulw::test::eventually([&] { return gw.claims() == 3; }));
    HttpClient setup(gw.port());
    const auto up = create_upload(setup, kMiB);
    ASSERT_TRUE(up);
    HttpClient fourth(gw.port());
    const auto r = patch(fourth, up->upload_id, 0, data);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 503);
    EXPECT_EQ(r->header("retry-after"), "5");
    EXPECT_EQ(gw.counters().admission_rejections, 1U);
    // Another user is not affected by alice's cap.
    HttpClient bob(gw.port());
    const auto theirs = create_upload(bob, kMiB, kBob);
    ASSERT_TRUE(theirs);
    EXPECT_EQ(patch(bob, theirs->upload_id, 0, data, kBob)->status, 204);
}

TEST(GatewayUpload, StalledBackendThrottlesTheClientInsteadOfBuffering) {
    GatewayOptions options{.backend = Backend::Fake, .chunk = 8 * kMiB};
    // Long enough that the stall is observed well before the gateway gives up on it.
    options.limits.body_idle_timeout = core::Millis{3'000};
    GatewayUnderTest gw(options);
    const auto data = ulw::test::pattern(8 * kMiB);
    HttpClient c(gw.port());
    const auto up = create_upload(c, data.size());
    ASSERT_TRUE(up);
    gw.set_plan({.accept_zero = true});

    HttpClient uploader(gw.port());
    timeval tv{.tv_sec = 0, .tv_usec = 200'000};
    ::setsockopt(uploader.fd(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    const std::string head = "PATCH /api/v1/uploads/" + up->upload_id +
                             " HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer user:alice\r\n"
                             "Upload-Offset: 0\r\nContent-Length: " +
                             std::to_string(data.size()) + "\r\n\r\n";
    ASSERT_TRUE(uploader.send_raw(head));
    // With nothing drained, the client's writes must stall once the kernel buffers on both
    // sides are full; the gateway itself holds at most its staging bound.
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n =
            ::send(uploader.fd(), std::next(data.data(), static_cast<std::ptrdiff_t>(sent)),
                   data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            break;
        }
        sent += static_cast<std::size_t>(n);
    }
    EXPECT_LT(sent, data.size());
    const auto ingested = gw.counters().bytes_ingested;
    EXPECT_LE(ingested, (std::uint64_t{256} * 1024) + (std::uint64_t{64} * 1024));

    // With no progress for the body timeout the gateway gives up on the store, answers 503
    // and releases everything the upload held, whether or not the client is still there.
    const auto r = uploader.read_response();
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 503);
    EXPECT_EQ(gw.counters().timeouts_backend, 1U);
    uploader.close();
    EXPECT_TRUE(ulw::test::eventually([&] { return gw.claims() == 0 && gw.connections() == 1; }));
}

TEST(GatewayUpload, FiftyConcurrentUploadsAllArriveIntact) {
    GatewayOptions options{.backend = Backend::Fs, .chunk = kMiB};
    options.limits.max_uploads_per_user = 64;
    GatewayUnderTest gw(options);
    constexpr int kUploads = 50;
    std::vector<std::jthread> threads;
    threads.reserve(kUploads);
    std::vector<int> ok(kUploads, 0);
    for (int i = 0; i < kUploads; ++i) {
        threads.emplace_back([&, i] {
            const auto data = ulw::test::pattern((3 * kMiB) + static_cast<std::size_t>(i),
                                                 static_cast<std::size_t>(i));
            HttpClient c(gw.port());
            const auto up = create_upload(c, data.size());
            if (!up || !upload_all(c, *up, data) ||
                c.request("POST", "/api/v1/uploads/" + up->upload_id + "/commit", kAlice)->status !=
                    200) {
                return;
            }
            const auto stored = gw.reader().fetch_small(
                *core::StorageKey::parse("videos/" + up->video_id + "/raw"), data.size());
            ok[static_cast<std::size_t>(i)] = stored && *stored == data ? 1 : 0;
        });
    }
    threads.clear();
    EXPECT_EQ(std::count(ok.begin(), ok.end(), 1), kUploads);
    EXPECT_EQ(gw.jobs().size(), static_cast<std::size_t>(kUploads));
}

TEST(GatewayUpload, PipelinedRequestsAreAnsweredInOrder) {
    const GatewayUnderTest gw({});
    HttpClient c(gw.port());
    ASSERT_TRUE(c.send_raw("GET /api/v1/healthz HTTP/1.1\r\nHost: t\r\n\r\n"
                           "GET /api/v1/readyz HTTP/1.1\r\nHost: t\r\n\r\n"
                           "GET /api/v1/nowhere HTTP/1.1\r\nHost: t\r\n\r\n"));
    EXPECT_EQ(c.read_response()->body, "ok\n");
    EXPECT_EQ(c.read_response()->body, "ready\n");
    EXPECT_EQ(c.read_response()->status, 404);
}

TEST(GatewayUpload, BadCreateRequestsAre400) {
    const GatewayUnderTest gw({.backend = Backend::Fake});
    HttpClient c(gw.port());
    for (const std::string_view body :
         {R"({"filename":"a.mp4","size_bytes":10,"content_type":"image/png"})",
          R"({"filename":"a.mp4","size_bytes":0,"content_type":"video/mp4"})",
          R"({"filename":"","size_bytes":10,"content_type":"video/mp4"})",
          R"({"filename":"a.mp4","size_bytes":"10","content_type":"video/mp4"})",
          R"({"filename":"a.mp4","size_bytes":10,"size_bytes":20,"content_type":"video/mp4"})",
          R"({"filename":"a.mp4","size_bytes":99999999999999,"content_type":"video/mp4"})",
          "not json"}) {
        const auto r = c.request("POST", "/api/v1/uploads", kAlice, std::as_bytes(std::span(body)));
        ASSERT_TRUE(r) << body;
        EXPECT_EQ(r->status, 400) << body;
    }
}

TEST(GatewayUpload, SmugglingAttemptIsRefusedAndClosed) {
    const GatewayUnderTest gw({});
    HttpClient c(gw.port());
    ASSERT_TRUE(c.send_raw("POST /api/v1/uploads HTTP/1.1\r\nHost: t\r\n"
                           "Content-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"));
    const auto r = c.read_response();
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_TRUE(c.closed_by_peer());
}

TEST(GatewayUpload, IdleConnectionIsClosedAfterTheHeaderTimeout) {
    GatewayOptions options{};
    options.limits.header_timeout = core::Millis{300};
    GatewayUnderTest gw(options);
    HttpClient c(gw.port());
    EXPECT_TRUE(c.closed_by_peer());
    EXPECT_GE(gw.counters().timeouts_header, 1U);
}

TEST(GatewayUpload, StalledBodyGets408) {
    GatewayOptions options{.backend = Backend::Fake, .chunk = kMiB};
    options.limits.body_idle_timeout = core::Millis{300};
    GatewayUnderTest gw(options);
    HttpClient c(gw.port());
    const auto up = create_upload(c, kMiB);
    ASSERT_TRUE(up);
    const std::string head = "PATCH /api/v1/uploads/" + up->upload_id +
                             " HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer user:alice\r\n"
                             "Upload-Offset: 0\r\nContent-Length: 1000\r\n\r\nabc";
    ASSERT_TRUE(c.send_raw(head));
    const auto r = c.read_response();
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 408);
    EXPECT_TRUE(c.closed_by_peer());
    EXPECT_TRUE(ulw::test::eventually([&] { return gw.claims() == 0; }));
}

TEST(GatewayUpload, DrainAnswersReadyzWith503AndClosesIdleConnections) {
    GatewayUnderTest gw({});
    HttpClient idle(gw.port());
    HttpClient busy(gw.port());
    ASSERT_EQ(busy.request("GET", "/api/v1/healthz", "")->status, 200);
    gw.drain();
    EXPECT_TRUE(idle.closed_by_peer());
    EXPECT_TRUE(busy.closed_by_peer());
    EXPECT_TRUE(ulw::test::eventually([&] { return gw.connections() == 0; }));
}

} // namespace
