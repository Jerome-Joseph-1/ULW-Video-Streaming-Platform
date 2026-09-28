// The store's control operations against a scripted S3 peer: which requests go out, and how
// each answer is read.
#include "core/ports/storage.hpp"
#include "infra/curl/multi.hpp"
#include "infra/s3util/credentials.hpp"
#include "infra/s3util/crypto.hpp"
#include "infra/s3util/profile.hpp"
#include "infra/s3util/sigv4.hpp"
#include "infra/storage/s3_store.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"

#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"
#include "support/http_test_server.hpp"

#include <deque>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using core::ports::IngestId;
using core::ports::StorageError;
using infra::storage::S3ConfigError;
using infra::storage::S3Store;
using ulw::test::Reply;
using ulw::test::ServedRequest;

constexpr std::uint64_t kPart = 4096;

std::string initiate_xml(std::string_view upload_id) {
    return R"(<?xml version="1.0" encoding="UTF-8"?><InitiateMultipartUploadResult>)"
           "<Bucket>media</Bucket><Key>k</Key><UploadId>" +
           std::string(upload_id) + "</UploadId></InitiateMultipartUploadResult>";
}

struct Listed {
    std::uint32_t number;
    std::uint64_t size;
};

std::string list_parts_xml(const std::vector<Listed>& parts, std::optional<std::uint32_t> next) {
    std::string xml = "<ListPartsResult><IsTruncated>";
    xml += next ? "true" : "false";
    xml += "</IsTruncated>";
    if (next) {
        xml += "<NextPartNumberMarker>" + std::to_string(*next) + "</NextPartNumberMarker>";
    }
    for (const auto& p : parts) {
        xml += "<Part><PartNumber>" + std::to_string(p.number) + "</PartNumber><ETag>&quot;etag-" +
               std::to_string(p.number) + "&quot;</ETag><Size>" + std::to_string(p.size) +
               "</Size></Part>";
    }
    return xml + "</ListPartsResult>";
}

std::string error_xml(std::string_view code) {
    return "<Error><Code>" + std::string(code) + "</Code><Message>m</Message></Error>";
}

Reply xml(int status, std::string body) {
    return Reply{.status = status, .headers = {}, .body = std::move(body)};
}

class S3StoreTest : public ::testing::Test {
protected:
    S3StoreTest()
        : server([this](const ServedRequest& r) {
              const std::scoped_lock lock(mutex);
              if (!script.empty()) {
                  Reply next = std::move(script.front());
                  script.pop_front();
                  return next;
              }
              return fallback(r);
          }) {}

    void SetUp() override {
        auto r = net::make_reactor(net::ReactorKind::Epoll, system_clock, 1024);
        ASSERT_TRUE(r);
        reactor = std::move(*r);
        auto m = infra::curl::Multi::create(*reactor);
        ASSERT_TRUE(m);
        multi = std::move(*m);
        auto s = make_store(kPart);
        ASSERT_TRUE(s);
        store = std::move(*s);
    }
    void TearDown() override {
        store.reset();
        multi.reset();
        reactor.reset();
    }

    [[nodiscard]] infra::s3util::S3Profile profile() const {
        auto p = *infra::s3util::S3Profile::minio(server.base_url());
        // Small parts keep the scripted listings readable.
        p.min_part_bytes = 1024;
        p.max_parts = 100;
        return p;
    }

    [[nodiscard]] std::expected<std::unique_ptr<S3Store>, S3ConfigError>
    make_store(std::uint64_t part_size, std::string bucket = "media") {
        return S3Store::create(
            S3Store::Deps{.reactor = *reactor,
                          .multi = *multi,
                          .credentials = credentials,
                          .clock = clock,
                          .random = random,
                          .profile = profile(),
                          .bucket = std::move(bucket)},
            infra::storage::S3StoreOptions{
                .part_size = part_size,
                // Millisecond backoff: the tests count retries, they do not time them.
                .retry = {.base = core::Millis{1}, .cap = core::Millis{2}, .max_retries = 3}});
    }

    void then(std::vector<Reply> replies) {
        const std::scoped_lock lock(mutex);
        for (auto& r : replies) {
            script.push_back(std::move(r));
        }
    }

    [[nodiscard]] static IngestId ingest(std::uint64_t total) {
        return IngestId{.key = *core::StorageKey::parse("videos/v1/raw"),
                        .backend_ref = "upload-1",
                        .total_bytes = total,
                        .chunk_size = kPart};
    }

    os::SystemClock system_clock;
    ulw::test::FakeClock clock;
    ulw::test::FakeRandom random;
    infra::s3util::StaticCredentialProvider credentials{
        *infra::s3util::Credentials::make("AKIDEXAMPLE", infra::s3util::SecretString("secret"))};
    std::mutex mutex;
    std::deque<Reply> script;
    std::function<Reply(const ServedRequest&)> fallback = [](const ServedRequest&) {
        return xml(500, error_xml("InternalError"));
    };
    ulw::test::HttpTestServer server;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<infra::curl::Multi> multi;
    std::unique_ptr<S3Store> store;
};

TEST_F(S3StoreTest, ConfigurationOutsideTheProfileIsRefused) {
    EXPECT_EQ(make_store(1023).error(), S3ConfigError::PartSizeOutOfRange);
    EXPECT_EQ(make_store(kPart, "Not_A_Bucket").error(), S3ConfigError::InvalidBucket);
    EXPECT_EQ(store->preferred_chunk_size(), kPart);
}

TEST_F(S3StoreTest, CreateStartsAMultipartUploadWhoseIdIsTheOpaqueRef) {
    then({xml(200, initiate_xml("upload-xyz"))});
    const auto id = store->create(*core::StorageKey::parse("videos/v1/raw"), 10'000,
                                  *core::ContentType::parse("video/mp4"));
    ASSERT_TRUE(id);
    EXPECT_EQ(id->backend_ref, "upload-xyz");
    EXPECT_EQ(id->key.str(), "videos/v1/raw");
    EXPECT_EQ(id->total_bytes, 10'000U);
    EXPECT_EQ(id->chunk_size, kPart);
    const auto sent = server.requests().at(0);
    EXPECT_EQ(sent.method, "POST");
    EXPECT_EQ(sent.target, "/media/videos/v1/raw?uploads=");
    EXPECT_EQ(sent.header("content-type"), "video/mp4");
    EXPECT_EQ(sent.header("x-amz-content-sha256"), infra::s3util::kEmptyPayloadSha256);
    // The content type is part of what was signed, so it cannot be swapped in transit.
    EXPECT_NE(sent.header("authorization")->find("content-type;"), std::string_view::npos);
}

TEST_F(S3StoreTest, ObjectLargerThanTheProfileAllowsIsRefusedWithoutARequest) {
    const auto id = store->create(*core::StorageKey::parse("videos/v1/raw"), (100 * kPart) + 1,
                                  *core::ContentType::parse("video/mp4"));
    EXPECT_EQ(id, std::unexpected(StorageError::Permanent));
    EXPECT_EQ(store->create(*core::StorageKey::parse("videos/v1/raw"), 0,
                            *core::ContentType::parse("video/mp4")),
              std::unexpected(StorageError::Permanent));
    EXPECT_EQ(server.request_count(), 0U);
}

TEST_F(S3StoreTest, ThrottledRequestIsRetriedUntilItSucceeds) {
    then({xml(503, error_xml("SlowDown")), xml(500, error_xml("InternalError")),
          xml(200, initiate_xml("u-2"))});
    const auto id = store->create(*core::StorageKey::parse("videos/v1/raw"), 10,
                                  *core::ContentType::parse("video/mp4"));
    ASSERT_TRUE(id);
    EXPECT_EQ(id->backend_ref, "u-2");
    EXPECT_EQ(server.request_count(), 3U);
}

TEST_F(S3StoreTest, RetryAfterBeyondTheBackoffCapGivesUpAtOnce) {
    then({Reply{.status = 503, .headers = {{"Retry-After", "60"}}, .body = error_xml("SlowDown")}});
    const auto id = store->create(*core::StorageKey::parse("videos/v1/raw"), 10,
                                  *core::ContentType::parse("video/mp4"));
    EXPECT_EQ(id, std::unexpected(StorageError::Throttled));
    EXPECT_EQ(server.request_count(), 1U);
}

TEST_F(S3StoreTest, RefusalIsNotRetried) {
    then({xml(403, error_xml("AccessDenied"))});
    const auto id = store->create(*core::StorageKey::parse("videos/v1/raw"), 10,
                                  *core::ContentType::parse("video/mp4"));
    EXPECT_EQ(id, std::unexpected(StorageError::Unauthorized));
    EXPECT_EQ(server.request_count(), 1U);
}

TEST_F(S3StoreTest, TransientFailuresStopWhenRetriesRunOut) {
    const auto id = store->create(*core::StorageKey::parse("videos/v1/raw"), 10,
                                  *core::ContentType::parse("video/mp4"));
    EXPECT_EQ(id, std::unexpected(StorageError::Transient));
    // The first attempt and max_retries more.
    EXPECT_EQ(server.request_count(), 4U);
}

TEST_F(S3StoreTest, DurableOffsetPagesThroughPartsAndStopsAtTheFirstGap) {
    then({xml(200, list_parts_xml({{.number = 1, .size = kPart}, {.number = 2, .size = kPart}}, 2)),
          xml(200, list_parts_xml({{.number = 3, .size = kPart}, {.number = 5, .size = kPart}},
                                  std::nullopt))});
    EXPECT_EQ(store->durable_offset(ingest(6 * kPart)), 3 * kPart);
    const auto sent = server.requests();
    ASSERT_EQ(sent.size(), 2U);
    EXPECT_EQ(sent[0].method, "GET");
    EXPECT_EQ(sent[0].query("uploadId"), "upload-1");
    EXPECT_EQ(sent[0].query("max-parts"), "1000");
    EXPECT_EQ(sent[0].query("part-number-marker"), std::nullopt);
    EXPECT_EQ(sent[1].query("part-number-marker"), "2");
}

TEST_F(S3StoreTest, PartOfTheWrongSizeIsNotDurable) {
    then({xml(200, list_parts_xml({{.number = 1, .size = kPart},
                                   {.number = 2, .size = kPart - 1},
                                   {.number = 3, .size = kPart}},
                                  std::nullopt))});
    EXPECT_EQ(store->durable_offset(ingest(3 * kPart)), kPart);
}

TEST_F(S3StoreTest, ShortLastPartCountsUpToTheObjectSize) {
    then({xml(200, list_parts_xml({{.number = 1, .size = kPart}, {.number = 2, .size = 100}},
                                  std::nullopt))});
    EXPECT_EQ(store->durable_offset(ingest(kPart + 100)), kPart + 100);
}

TEST_F(S3StoreTest, DurableOffsetOfACompletedUploadIsItsSize) {
    then({xml(404, error_xml("NoSuchUpload")),
          Reply{.status = 200,
                .headers = {{"Content-Length", std::to_string(2 * kPart)}},
                .body = {}}});
    EXPECT_EQ(store->durable_offset(ingest(2 * kPart)), 2 * kPart);
    EXPECT_EQ(server.requests().at(1).method, "HEAD");
}

TEST_F(S3StoreTest, CommitCompletesWithTheListedETagsInOrderAndASignedBodyHash) {
    then({xml(200, list_parts_xml({{.number = 1, .size = kPart},
                                   {.number = 2, .size = kPart},
                                   {.number = 3, .size = 7}},
                                  std::nullopt)),
          xml(200, "<CompleteMultipartUploadResult><ETag>&quot;x-3&quot;</ETag>"
                   "</CompleteMultipartUploadResult>")});
    ASSERT_TRUE(store->commit(ingest((2 * kPart) + 7)));
    const auto sent = server.requests();
    ASSERT_EQ(sent.size(), 2U);
    const auto& complete = sent[1];
    EXPECT_EQ(complete.method, "POST");
    EXPECT_EQ(complete.target, "/media/videos/v1/raw?uploadId=upload-1");
    EXPECT_EQ(complete.header("x-amz-content-sha256"),
              infra::s3util::payload_sha256(std::as_bytes(std::span(complete.body))));
    const auto p1 = complete.body.find("<PartNumber>1</PartNumber><ETag>&quot;etag-1&quot;</ETag>");
    const auto p2 = complete.body.find("<PartNumber>2</PartNumber><ETag>&quot;etag-2&quot;</ETag>");
    const auto p3 = complete.body.find("<PartNumber>3</PartNumber><ETag>&quot;etag-3&quot;</ETag>");
    ASSERT_NE(p1, std::string::npos);
    ASSERT_NE(p2, std::string::npos);
    ASSERT_NE(p3, std::string::npos);
    EXPECT_LT(p1, p2);
    EXPECT_LT(p2, p3);
}

TEST_F(S3StoreTest, CommitWithAMissingPartCompletesNothing) {
    then({xml(200, list_parts_xml({{.number = 1, .size = kPart}, {.number = 3, .size = kPart}},
                                  std::nullopt))});
    EXPECT_EQ(store->commit(ingest(3 * kPart)), std::unexpected(StorageError::PreconditionFailed));
    EXPECT_EQ(server.request_count(), 1U);
}

TEST_F(S3StoreTest, ErrorInsideA200IsRetriedAndALostCompletionIsReconciled) {
    then(
        {xml(200, list_parts_xml({{.number = 1, .size = kPart}}, std::nullopt)),
         xml(200, error_xml("InternalError")), xml(404, error_xml("NoSuchUpload")),
         Reply{.status = 200, .headers = {{"Content-Length", std::to_string(kPart)}}, .body = {}}});
    EXPECT_TRUE(store->commit(ingest(kPart)));
    const auto sent = server.requests();
    ASSERT_EQ(sent.size(), 4U);
    EXPECT_EQ(sent[1].method, "POST");
    EXPECT_EQ(sent[2].method, "POST");
    EXPECT_EQ(sent[3].method, "HEAD");
}

TEST_F(S3StoreTest, CommitOfAnUploadThatIsGoneWithNoObjectIsNotFound) {
    then({xml(404, error_xml("NoSuchUpload")), xml(404, {})});
    EXPECT_EQ(store->commit(ingest(kPart)), std::unexpected(StorageError::NotFound));
}

TEST_F(S3StoreTest, ObjectOfAnotherSizeDoesNotReconcileACommit) {
    then({xml(404, error_xml("NoSuchUpload")),
          Reply{.status = 200, .headers = {{"Content-Length", "1"}}, .body = {}}});
    EXPECT_EQ(store->commit(ingest(kPart)), std::unexpected(StorageError::NotFound));
}

TEST_F(S3StoreTest, DiscardAbortsAndTakesNoSuchUploadAsDone) {
    then({xml(404, error_xml("NoSuchUpload"))});
    store->discard(ingest(kPart));
    const auto sent = server.requests();
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(sent[0].method, "DELETE");
    EXPECT_EQ(sent[0].target, "/media/videos/v1/raw?uploadId=upload-1");
}

TEST_F(S3StoreTest, OpenAcceptsOnlyPartBoundariesAndTheEnd) {
    struct Quiet final : core::ports::IIngestObserver {
        void on_ingest_progress() noexcept override {}
    } observer;
    const IngestId id = ingest((2 * kPart) + 10);
    EXPECT_TRUE(store->open(id, 0, observer));
    EXPECT_TRUE(store->open(id, kPart, observer));
    EXPECT_TRUE(store->open(id, id.total_bytes, observer));
    EXPECT_EQ(store->open(id, kPart + 1, observer).error(), StorageError::PreconditionFailed);
    EXPECT_EQ(store->open(id, id.total_bytes + 1, observer).error(),
              StorageError::PreconditionFailed);
    IngestId foreign = id;
    foreign.chunk_size = 0;
    EXPECT_EQ(store->open(foreign, 0, observer).error(), StorageError::NotFound);
    EXPECT_EQ(store->durable_offset(foreign), std::unexpected(StorageError::NotFound));
    EXPECT_EQ(server.request_count(), 0U);
}

} // namespace
