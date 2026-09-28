#include "core/models/storage_key.hpp"
#include "infra/s3util/profile.hpp"
#include "infra/s3util/url.hpp"

#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <vector>

namespace {

using infra::s3util::Addressing;
using infra::s3util::Bucket;
using infra::s3util::canonical_query;
using infra::s3util::ProfileError;
using infra::s3util::QueryParam;
using infra::s3util::S3Profile;
using infra::s3util::to_url;
using infra::s3util::uri_encode;
using infra::s3util::uri_encode_path;

core::StorageKey key(std::string_view text) {
    return core::StorageKey::parse(text).value();
}

S3Profile virtual_hosted(std::string_view endpoint) {
    S3Profile profile = S3Profile::minio(endpoint).value();
    profile.addressing = Addressing::VirtualHosted;
    return profile;
}

TEST(UriEncode, LeavesOnlyUnreservedCharactersBare) {
    EXPECT_EQ(uri_encode("AZaz09-_.~"), "AZaz09-_.~");
    EXPECT_EQ(uri_encode(" "), "%20");
    EXPECT_EQ(uri_encode("/"), "%2F");
    EXPECT_EQ(uri_encode("+"), "%2B");
    EXPECT_EQ(uri_encode("*"), "%2A");
    EXPECT_EQ(uri_encode("%"), "%25");
    EXPECT_EQ(uri_encode("="), "%3D");
    EXPECT_EQ(uri_encode("&"), "%26");
    EXPECT_EQ(uri_encode(std::string_view("\0", 1)), "%00");
}

TEST(UriEncode, EncodesEachUtf8ByteInUppercaseHex) {
    EXPECT_EQ(uri_encode("caf\xc3\xa9"), "caf%C3%A9");
    EXPECT_EQ(uri_encode("\xe2\x82\xac"), "%E2%82%AC");
    EXPECT_EQ(uri_encode("\xff"), "%FF");
}

TEST(UriEncode, PathFormKeepsSlashesAndEncodesEverythingElseOnce) {
    EXPECT_EQ(uri_encode_path("/a b/c~d/e+f"), "/a%20b/c~d/e%2Bf");
    EXPECT_EQ(uri_encode_path("/test$file.text"), "/test%24file.text");
    EXPECT_EQ(uri_encode_path("/already%20encoded"), "/already%2520encoded");
}

TEST(CanonicalQuery, GivesFlagParametersAnEqualsSign) {
    const std::vector<QueryParam> q{{.name = "uploads", .value = ""}};
    EXPECT_EQ(canonical_query(q), "uploads=");
}

TEST(CanonicalQuery, SortsByEncodedNameThenValue) {
    const std::vector<QueryParam> q{{.name = "prefix", .value = "J"},
                                    {.name = "max-keys", .value = "2"},
                                    {.name = "a", .value = "2"},
                                    {.name = "a", .value = "1"}};
    EXPECT_EQ(canonical_query(q), "a=1&a=2&max-keys=2&prefix=J");

    // Raw order puts 0xC3 after 'z'; encoded, "%C3" sorts before every letter.
    const std::vector<QueryParam> utf8{{.name = "z", .value = ""},
                                       {.name = "\xc3\xa9", .value = ""}};
    EXPECT_EQ(canonical_query(utf8), "%C3%A9=&z=");
}

TEST(CanonicalQuery, EncodesSlashesAndSpacesInValues) {
    const std::vector<QueryParam> q{{.name = "prefix", .value = "raw/a b"},
                                    {.name = "continuation-token", .value = "x+y=="}};
    EXPECT_EQ(canonical_query(q), "continuation-token=x%2By%3D%3D&prefix=raw%2Fa%20b");
}

TEST(Bucket, PathStylePutsTheBucketInThePath) {
    const auto profile = S3Profile::minio("http://localhost:9000").value();
    const auto bucket = Bucket::make(profile, "media.raw");
    ASSERT_TRUE(bucket.has_value());

    const auto target =
        bucket->object(key("raw/v1/source.mp4"), {{.name = "uploadId", .value = "abc/def"},
                                                  {.name = "partNumber", .value = "7"}});
    EXPECT_EQ(target.host, "localhost:9000");
    EXPECT_EQ(target.path, "/media.raw/raw/v1/source.mp4");
    EXPECT_EQ(to_url(target),
              "http://localhost:9000/media.raw/raw/v1/source.mp4?partNumber=7&uploadId=abc%2Fdef");

    const auto root = bucket->root({{.name = "list-type", .value = "2"}});
    EXPECT_EQ(root.path, "/media.raw");
    EXPECT_EQ(to_url(root), "http://localhost:9000/media.raw?list-type=2");
}

TEST(Bucket, VirtualHostedPutsTheBucketInTheHost) {
    const auto bucket = Bucket::make(virtual_hosted("https://storage.example"), "media");
    ASSERT_TRUE(bucket.has_value());

    const auto target = bucket->object(key("hls/v1/720p/seg_00001.m4s"));
    EXPECT_EQ(target.host, "media.storage.example");
    EXPECT_EQ(target.path, "/hls/v1/720p/seg_00001.m4s");
    EXPECT_EQ(to_url(target), "https://media.storage.example/hls/v1/720p/seg_00001.m4s");

    const auto root = bucket->root({{.name = "uploads", .value = ""}});
    EXPECT_EQ(root.path, "/");
    EXPECT_EQ(to_url(root), "https://media.storage.example/?uploads=");
}

TEST(Bucket, RejectsNamesThatCouldEscapeTheHostOrPath) {
    const auto profile = S3Profile::minio("http://localhost:9000").value();
    for (const std::string_view name :
         {"", "ab", "Media", "a..b", "-media", "media-", ".media", "media.", "me/dia", "me dia",
          "me%2Fdia", "media?x", "media#x", "me_dia"}) {
        EXPECT_EQ(Bucket::make(profile, name).error_or(ProfileError::InvalidEndpoint),
                  ProfileError::InvalidBucket)
            << name;
    }
    EXPECT_TRUE(Bucket::make(profile, std::string(63, 'm')).has_value());
    EXPECT_FALSE(Bucket::make(profile, std::string(64, 'm')).has_value());
}

TEST(Bucket, RefusesDottedNamesOnlyWhenTheyWouldBecomeAHostLabel) {
    const auto path_style = S3Profile::minio("http://localhost:9000").value();
    EXPECT_TRUE(Bucket::make(path_style, "media.example").has_value());
    EXPECT_FALSE(
        Bucket::make(virtual_hosted("https://storage.example"), "media.example").has_value());
}

} // namespace
