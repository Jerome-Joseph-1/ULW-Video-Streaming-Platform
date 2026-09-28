#include "infra/curl/http.hpp"

#include "support/http_test_server.hpp"
#include "support/reactor_harness.hpp"

#include <algorithm>
#include <cstddef>
#include <gtest/gtest.h>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

using infra::curl::FailureKind;
using infra::curl::Method;
using infra::curl::perform;
using infra::curl::Request;
using infra::curl::Response;
using ulw::test::HttpTestServer;
using ulw::test::kMiB;
using ulw::test::Reply;
using ulw::test::ServedRequest;

Request request(Method method, std::string url, std::vector<std::string> headers = {},
                std::size_t max_body = 1024) {
    return Request{.method = method,
                   .url = std::move(url),
                   .headers = std::move(headers),
                   .max_body = max_body};
}

TEST(HeaderLookup, IsCaseInsensitiveTrimmedAndTakesTheFirstOccurrence) {
    Response r;
    r.headers = "HTTP/1.1 200 OK\r\nETag: \"a\"\r\nX-Dup: one \r\nx-dup: two\r\nEmpty:\r\n\r\n";
    EXPECT_EQ(r.header("etag"), "\"a\"");
    EXPECT_EQ(r.header("ETAG"), "\"a\"");
    EXPECT_EQ(r.header("x-dup"), "one");
    EXPECT_EQ(r.header("empty"), "");
    EXPECT_EQ(r.header("etag2"), std::nullopt);
    // A name that is a prefix of a real header must not match it.
    EXPECT_EQ(r.header("x-du"), std::nullopt);
}

TEST(Perform, PutSendsTheWholeBodyWithItsLengthAndNoExpect) {
    const HttpTestServer server([](const ServedRequest&) {
        return Reply{.status = 200, .headers = {{"ETag", "\"etag-1\""}}, .body = {}};
    });
    const auto body = ulw::test::pattern(2 * kMiB);
    const auto result =
        perform(request(Method::Put, server.base_url() + "/obj?x=1", {"x-amz-date: now"}), body);
    ASSERT_TRUE(result) << result.error().detail;
    EXPECT_EQ(result->status, 200);
    EXPECT_EQ(result->header("etag"), "\"etag-1\"");
    const auto served = server.requests().at(0);
    EXPECT_EQ(served.method, "PUT");
    EXPECT_EQ(served.target, "/obj?x=1");
    EXPECT_EQ(served.header("content-length"), std::to_string(body.size()));
    EXPECT_EQ(served.header("x-amz-date"), "now");
    EXPECT_EQ(served.header("expect"), std::nullopt);
    EXPECT_TRUE(std::ranges::equal(std::as_bytes(std::span(served.body)), body));
}

TEST(Perform, PostWithoutABodyDeclaresZeroLength) {
    const HttpTestServer server([](const ServedRequest&) { return Reply{}; });
    const auto result = perform(
        request(Method::Post, server.base_url() + "/obj?uploads", {"content-type: video/mp4"}));
    ASSERT_TRUE(result) << result.error().detail;
    const auto served = server.requests().at(0);
    EXPECT_EQ(served.method, "POST");
    EXPECT_EQ(served.header("content-length"), "0");
    EXPECT_EQ(served.header("content-type"), "video/mp4");
}

TEST(Perform, HeadReturnsHeadersWithoutWaitingForABody) {
    const HttpTestServer server([](const ServedRequest&) {
        return Reply{.status = 200, .headers = {{"Content-Length", "12345"}}, .body = {}};
    });
    const auto result = perform(request(Method::Head, server.base_url() + "/obj"));
    ASSERT_TRUE(result) << result.error().detail;
    EXPECT_EQ(result->status, 200);
    EXPECT_EQ(result->header("content-length"), "12345");
    EXPECT_TRUE(result->body.empty());
    EXPECT_EQ(server.requests().at(0).method, "HEAD");
}

TEST(Perform, DeleteIsSentAsDelete) {
    const HttpTestServer server(
        [](const ServedRequest&) { return Reply{.status = 204, .headers = {}, .body = {}}; });
    const auto result = perform(request(Method::Delete, server.base_url() + "/obj"));
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 204);
    EXPECT_EQ(server.requests().at(0).method, "DELETE");
}

TEST(Perform, HeaderWithALineBreakIsRefusedBeforeAnythingIsSent) {
    const HttpTestServer server([](const ServedRequest&) { return Reply{}; });
    const auto result =
        perform(request(Method::Get, server.base_url() + "/", {"x-a: 1\r\nx-injected: 2"}));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, FailureKind::Local);
    EXPECT_EQ(server.request_count(), 0U);
}

TEST(Perform, OnlyHttpAndHttpsAreAllowed) {
    const auto result = perform(request(Method::Get, "file:///etc/passwd"));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, FailureKind::Local);
}

TEST(Perform, ConcurrentCallsFromManyThreadsAreIndependent) {
    const HttpTestServer server([](const ServedRequest& r) {
        return Reply{.status = 200, .headers = {}, .body = r.target};
    });
    constexpr int kThreads = 8;
    std::vector<std::string> bodies(kThreads);
    {
        std::vector<std::jthread> threads;
        threads.reserve(kThreads);
        for (int i = 0; i < kThreads; ++i) {
            threads.emplace_back([&, i] {
                const auto r =
                    perform(request(Method::Get, server.base_url() + "/t" + std::to_string(i)));
                bodies[static_cast<std::size_t>(i)] = r ? r->body : "failed";
            });
        }
    }
    for (int i = 0; i < kThreads; ++i) {
        EXPECT_EQ(bodies[static_cast<std::size_t>(i)], "/t" + std::to_string(i));
    }
}

std::string as_text(std::span<const std::byte> bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (const std::byte b : bytes) {
        out.push_back(static_cast<char>(b));
    }
    return out;
}

// Keeps what arrives and how many pieces it came in; refuses everything after `accept` bytes.
struct CollectingSink final : infra::curl::IDownloadSink {
    std::vector<std::byte> bytes;
    std::size_t pieces = 0;
    std::size_t accept = std::size_t(-1);

    bool write_download(std::span<const std::byte> piece) noexcept override {
        if (bytes.size() + piece.size() > accept) {
            return false;
        }
        bytes.insert(bytes.end(), piece.begin(), piece.end());
        ++pieces;
        return true;
    }
};

// Hands out `data`, a little at a time, and then runs dry.
struct SpanUpload final : infra::curl::IUploadSource {
    std::span<const std::byte> data;
    std::size_t max_piece = std::size_t{64} * 1024;

    std::size_t read_upload(std::span<std::byte> out) noexcept override {
        const std::size_t n = std::min({out.size(), data.size(), max_piece});
        std::ranges::copy(data.first(n), out.begin());
        data = data.subspan(n);
        return n;
    }
};

TEST(PerformDownload, StreamsALargeBodyToTheSinkAndKeepsNoneOfIt) {
    const auto body = ulw::test::pattern(3 * kMiB);
    const HttpTestServer server([&](const ServedRequest&) {
        return Reply{.status = 200, .headers = {}, .body = as_text(body)};
    });
    CollectingSink sink;
    // max_body bounds only bodies kept in memory; far smaller than what streams through.
    const auto result =
        infra::curl::perform_download(request(Method::Get, server.base_url() + "/raw"), sink);
    ASSERT_TRUE(result) << result.error().detail;
    EXPECT_EQ(result->status, 200);
    EXPECT_TRUE(result->body.empty());
    EXPECT_TRUE(std::ranges::equal(sink.bytes, body));
    EXPECT_GT(sink.pieces, 1U);
}

TEST(PerformDownload, KeepsAnErrorBodyForTheCallerAndNeverFeedsTheSink) {
    const HttpTestServer server([](const ServedRequest&) {
        return Reply{.status = 404, .headers = {}, .body = "<Error><Code>NoSuchKey</Code></Error>"};
    });
    CollectingSink sink;
    const auto result =
        infra::curl::perform_download(request(Method::Get, server.base_url() + "/raw"), sink);
    ASSERT_TRUE(result) << result.error().detail;
    EXPECT_EQ(result->status, 404);
    EXPECT_EQ(result->body, "<Error><Code>NoSuchKey</Code></Error>");
    EXPECT_TRUE(sink.bytes.empty());
}

TEST(PerformDownload, ASinkThatRefusesAbortsTheExchange) {
    const auto body = ulw::test::pattern(2 * kMiB);
    const HttpTestServer server([&](const ServedRequest&) {
        return Reply{.status = 200, .headers = {}, .body = as_text(body)};
    });
    CollectingSink sink;
    sink.accept = kMiB;
    const auto result =
        infra::curl::perform_download(request(Method::Get, server.base_url() + "/raw"), sink);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, FailureKind::Local);
    EXPECT_LE(sink.bytes.size(), kMiB);
}

TEST(PerformUpload, SendsExactlyTheDeclaredLengthFromTheSource) {
    const HttpTestServer server([](const ServedRequest&) { return Reply{}; });
    const auto body = ulw::test::pattern((3 * kMiB) + 17);
    SpanUpload source;
    source.data = body;
    const auto result = infra::curl::perform_upload(
        request(Method::Put, server.base_url() + "/seg"), body.size(), source);
    ASSERT_TRUE(result) << result.error().detail;
    const auto served = server.requests().at(0);
    EXPECT_EQ(served.header("content-length"), std::to_string(body.size()));
    EXPECT_TRUE(served.complete);
    EXPECT_TRUE(std::ranges::equal(std::as_bytes(std::span(served.body)), body));
}

TEST(PerformUpload, ASourceThatRunsDryAbortsInsteadOfWaitingForever) {
    const HttpTestServer server([](const ServedRequest&) { return Reply{}; });
    const auto body = ulw::test::pattern(kMiB);
    SpanUpload source;
    source.data = body;
    // Declares twice what the source holds, as a file truncated under the reader would.
    const auto result = infra::curl::perform_upload(
        request(Method::Put, server.base_url() + "/seg"), 2 * body.size(), source);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, FailureKind::Local);
}

} // namespace
