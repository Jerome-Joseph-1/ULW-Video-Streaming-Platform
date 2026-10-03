// What the S3 adapter assumes about the wire, checked against a real MinIO with raw signed
// requests rather than through the adapter. Each test pins one behaviour the adapter's
// design depends on; if one of these changes, so must the adapter.
#include "core/models/storage_key.hpp"
#include "infra/curl/http.hpp"
#include "infra/s3util/sigv4.hpp"
#include "infra/s3util/url.hpp"
#include "infra/s3util/xml.hpp"

#include "support/live_s3.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <gtest/gtest.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using infra::curl::Method;
using infra::curl::Response;
using infra::s3util::Header;
using infra::s3util::QueryParam;

// S3's floor for every part but the last.
constexpr std::size_t kMinPart = std::size_t{5} << 20U;

std::string error_code(const Response& r) {
    const auto doc = infra::s3util::parse_error_document(r.body);
    return doc ? doc->code : std::string();
}

std::span<const std::byte> bytes(const std::string& s) {
    return std::as_bytes(std::span(s));
}

class S3WireClaims : public ::testing::Test {
protected:
    void SetUp() override {
        if (!ulw::test::ensure_bucket(target_)) {
            const std::string where =
                target_.profile.endpoint.host + ":" + std::to_string(target_.profile.endpoint.port);
#ifdef ULW_CONFORMANCE_LIVE
            FAIL() << "MinIO unreachable at " << where;
#else
            GTEST_SKIP() << "MinIO unreachable at " << where
                         << "; start deploy/local/compose.yaml or set "
                            "ULW_MINIO_ENDPOINT";
#endif
        }
        bucket_ = *infra::s3util::Bucket::make(target_.profile, target_.bucket);
        prefix_ = ulw::test::unique_prefix("wire");
    }

    void TearDown() override {
        for (const auto& [key, id] : uploads_) {
            static_cast<void>(
                send(Method::Delete, bucket_->object(key, {{.name = "uploadId", .value = id}})));
        }
        for (const auto& key : objects_) {
            static_cast<void>(send(Method::Delete, bucket_->object(key)));
        }
    }

    [[nodiscard]] core::StorageKey key(std::string_view name) {
        auto k = *core::StorageKey::parse(prefix_ + std::string(name));
        objects_.push_back(k);
        return k;
    }

    [[nodiscard]] Response send(Method method, const infra::s3util::RequestTarget& t,
                                std::span<const std::byte> body = {},
                                std::span<const Header> headers = {},
                                std::optional<std::string_view> payload_hash = {}) const {
        auto r = ulw::test::send(target_, method, t, body, headers, payload_hash);
        EXPECT_TRUE(r) << r.error().detail;
        return r ? *std::move(r) : Response{};
    }

    std::string initiate(const core::StorageKey& k) {
        const Response r =
            send(Method::Post, bucket_->object(k, {{.name = "uploads", .value = ""}}));
        EXPECT_EQ(r.status, 200) << r.body;
        auto parsed = infra::s3util::parse_initiate_multipart_upload(r.body);
        EXPECT_TRUE(parsed);
        std::string id = parsed ? parsed->upload_id : std::string();
        uploads_.emplace_back(k, id);
        return id;
    }

    // Streamed the way the adapter streams: the body hash is not sent.
    [[nodiscard]] Response upload_part(const core::StorageKey& k, const std::string& id, int number,
                                       const std::string& body) const {
        return send(Method::Put,
                    bucket_->object(k, {{.name = "partNumber", .value = std::to_string(number)},
                                        {.name = "uploadId", .value = id}}),
                    bytes(body), {}, infra::s3util::kUnsignedPayload);
    }

    // A part's ETag, or a failure naming what the store answered instead: dereferencing an
    // absent header would be undefined behaviour, not a test failure.
    static std::string etag_of(const Response& part) {
        const auto etag = part.header("etag");
        if (!etag) {
            ADD_FAILURE() << "no ETag on a part upload: status " << part.status << "\n"
                          << part.body;
            return {};
        }
        return std::string(*etag);
    }

    Response complete(const core::StorageKey& k, const std::string& id, const std::string& body) {
        const std::array headers{Header{.name = "content-type", .value = "application/xml"}};
        return send(Method::Post, bucket_->object(k, {{.name = "uploadId", .value = id}}),
                    bytes(body), headers);
    }

    // Parts in exactly the order given, which the library's own builder would not allow.
    static std::string completion(const std::vector<std::pair<int, std::string>>& parts) {
        std::string xml = "<CompleteMultipartUpload>";
        for (const auto& [number, etag] : parts) {
            xml += "<Part><PartNumber>" + std::to_string(number) + "</PartNumber><ETag>" + etag +
                   "</ETag></Part>";
        }
        return xml + "</CompleteMultipartUpload>";
    }

    [[nodiscard]] infra::s3util::ListPartsResult list_parts(const core::StorageKey& k,
                                                            const std::string& id, int max_parts,
                                                            std::optional<int> marker) const {
        std::vector<QueryParam> query{{.name = "uploadId", .value = id},
                                      {.name = "max-parts", .value = std::to_string(max_parts)}};
        if (marker) {
            query.push_back({.name = "part-number-marker", .value = std::to_string(*marker)});
        }
        const Response r = send(Method::Get, bucket_->object(k, std::move(query)));
        EXPECT_EQ(r.status, 200) << r.body;
        auto parsed = infra::s3util::parse_list_parts(r.body);
        EXPECT_TRUE(parsed);
        return parsed ? *std::move(parsed) : infra::s3util::ListPartsResult{};
    }

    ulw::test::LiveS3 target_{ulw::test::minio_from_env()};
    std::optional<infra::s3util::Bucket> bucket_;
    std::string prefix_;
    std::vector<std::pair<core::StorageKey, std::string>> uploads_;
    std::vector<core::StorageKey> objects_;
};

TEST_F(S3WireClaims, UnsignedPayloadPartIsAcceptedAndAnsweredWithAnETag) {
    const auto k = key("unsigned");
    const auto id = initiate(k);
    const Response r = upload_part(k, id, 1, std::string(1024, 'u'));
    EXPECT_EQ(r.status, 200) << r.body;
    const auto etag = r.header("etag");
    ASSERT_TRUE(etag);
    EXPECT_GT(etag->size(), 2U);
    EXPECT_TRUE(etag->starts_with('"') && etag->ends_with('"'));
}

TEST_F(S3WireClaims, SmallFinalPartIsAcceptedAndMultipartETagEndsInThePartCount) {
    const auto k = key("small-final");
    const auto id = initiate(k);
    const Response p1 = upload_part(k, id, 1, std::string(kMinPart, 'a'));
    const Response p2 = upload_part(k, id, 2, std::string(1024, 'b'));
    ASSERT_EQ(p1.status, 200);
    ASSERT_EQ(p2.status, 200);
    const Response done = complete(k, id, completion({{1, etag_of(p1)}, {2, etag_of(p2)}}));
    ASSERT_EQ(done.status, 200) << done.body;
    const auto result = infra::s3util::parse_complete_multipart_upload(done.body);
    ASSERT_TRUE(result);
    EXPECT_TRUE(result->etag.ends_with("-2\"")) << result->etag;
    const Response head = send(Method::Head, bucket_->object(k));
    EXPECT_EQ(head.header("content-length"), std::to_string(kMinPart + 1024));
    EXPECT_TRUE(head.header("etag")->ends_with("-2\""));
}

TEST_F(S3WireClaims, ListPartsPagesByMaxPartsWithANextMarker) {
    const auto k = key("paging");
    const auto id = initiate(k);
    for (int n = 1; n <= 3; ++n) {
        ASSERT_EQ(upload_part(k, id, n, std::string(100, static_cast<char>('0' + n))).status, 200);
    }
    const auto first = list_parts(k, id, 2, std::nullopt);
    EXPECT_TRUE(first.is_truncated);
    EXPECT_EQ(first.next_part_number_marker, 2U);
    ASSERT_EQ(first.parts.size(), 2U);
    EXPECT_EQ(first.parts[0].part_number, 1U);
    EXPECT_EQ(first.parts[1].part_number, 2U);
    const auto second = list_parts(k, id, 2, 2);
    EXPECT_FALSE(second.is_truncated);
    ASSERT_EQ(second.parts.size(), 1U);
    EXPECT_EQ(second.parts[0].part_number, 3U);
    EXPECT_EQ(second.parts[0].size, 100U);
}

TEST_F(S3WireClaims, PartsOutOfOrderAreRefusedWithInvalidPartOrder) {
    const auto k = key("reversed");
    const auto id = initiate(k);
    const Response p1 = upload_part(k, id, 1, std::string(kMinPart, 'a'));
    const Response p2 = upload_part(k, id, 2, std::string(10, 'b'));
    const Response done = complete(k, id, completion({{2, etag_of(p2)}, {1, etag_of(p1)}}));
    EXPECT_EQ(done.status, 400);
    EXPECT_EQ(error_code(done), "InvalidPartOrder");
}

TEST_F(S3WireClaims, RetriedCompleteFindsNoSuchUploadAndHeadShowsTheObject) {
    const auto k = key("retried");
    const auto id = initiate(k);
    const Response p1 = upload_part(k, id, 1, std::string(4096, 'r'));
    const std::string body = completion({{1, etag_of(p1)}});
    ASSERT_EQ(complete(k, id, body).status, 200);
    const Response again = complete(k, id, body);
    EXPECT_EQ(again.status, 404);
    EXPECT_EQ(error_code(again), "NoSuchUpload");
    const Response head = send(Method::Head, bucket_->object(k));
    EXPECT_EQ(head.status, 200);
    EXPECT_EQ(head.header("content-length"), "4096");
}

TEST_F(S3WireClaims, UndersizedNonFinalPartFailsAtCompleteNotAtUpload) {
    const auto k = key("too-small");
    const auto id = initiate(k);
    const Response p1 = upload_part(k, id, 1, std::string(1024, 'x'));
    const Response p2 = upload_part(k, id, 2, std::string(1024, 'y'));
    EXPECT_EQ(p1.status, 200);
    EXPECT_EQ(p2.status, 200);
    const Response done = complete(k, id, completion({{1, etag_of(p1)}, {2, etag_of(p2)}}));
    EXPECT_EQ(done.status, 400);
    EXPECT_EQ(error_code(done), "EntityTooSmall");
}

TEST_F(S3WireClaims, AbortAnswers204AndTheUploadIsGone) {
    const auto k = key("abort");
    const auto id = initiate(k);
    const auto target = bucket_->object(k, {{.name = "uploadId", .value = id}});
    EXPECT_EQ(send(Method::Delete, target).status, 204);
    // A second abort is 204 again on MinIO and 404 NoSuchUpload on AWS; discard takes both.
    const Response again = send(Method::Delete, target);
    EXPECT_TRUE(again.status == 204 || (again.status == 404 && error_code(again) == "NoSuchUpload"))
        << again.status << " " << again.body;
    const Response parts = send(Method::Get, target);
    EXPECT_EQ(parts.status, 404);
    EXPECT_EQ(error_code(parts), "NoSuchUpload");
}

TEST_F(S3WireClaims, OrphanedUploadIsListedAsAnUploadButNotAsAnObject) {
    const auto k = key("orphan");
    const auto id = initiate(k);
    ASSERT_EQ(upload_part(k, id, 1, std::string(100, 'o')).status, 200);
    const Response uploads = send(
        Method::Get,
        bucket_->root({{.name = "uploads", .value = ""}, {.name = "prefix", .value = k.str()}}));
    const auto listed = infra::s3util::parse_list_multipart_uploads(uploads.body);
    ASSERT_TRUE(listed);
    EXPECT_TRUE(std::ranges::any_of(
        listed->uploads, [&](const auto& u) { return u.upload_id == id && u.key == k.str(); }));
    const Response objects = send(
        Method::Get,
        bucket_->root({{.name = "list-type", .value = "2"}, {.name = "prefix", .value = k.str()}}));
    const auto keys = infra::s3util::parse_list_objects_v2(objects.body);
    ASSERT_TRUE(keys);
    EXPECT_TRUE(keys->keys.empty());
}

TEST_F(S3WireClaims, UploadingAPartAgainReplacesIt) {
    const auto k = key("replace");
    const auto id = initiate(k);
    const Response first = upload_part(k, id, 1, "first attempt");
    const Response second = upload_part(k, id, 1, "second");
    ASSERT_EQ(first.status, 200);
    ASSERT_EQ(second.status, 200);
    EXPECT_NE(first.header("etag"), second.header("etag"));
    const auto parts = list_parts(k, id, 1000, std::nullopt);
    ASSERT_EQ(parts.parts.size(), 1U);
    EXPECT_EQ(parts.parts[0].size, std::string("second").size());
    EXPECT_EQ(parts.parts[0].etag, *second.header("etag"));
    ASSERT_EQ(complete(k, id, completion({{1, parts.parts[0].etag}})).status, 200);
    const Response object = send(Method::Get, bucket_->object(k));
    EXPECT_EQ(object.body, "second");
}

} // namespace
