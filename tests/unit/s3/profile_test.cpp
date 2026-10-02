#include "infra/s3util/profile.hpp"

#include <chrono>
#include <gtest/gtest.h>
#include <string_view>

namespace {

using infra::s3util::Addressing;
using infra::s3util::authority;
using infra::s3util::parse_endpoint;
using infra::s3util::ProfileError;
using infra::s3util::S3Profile;
using infra::s3util::Scheme;

TEST(S3Profile, MinioIsPathStyleInTheDefaultRegionOnTheGivenEndpoint) {
    const auto p = S3Profile::minio("http://minio.internal:9000");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->endpoint.scheme, Scheme::Http);
    EXPECT_EQ(p->endpoint.host, "minio.internal");
    EXPECT_EQ(p->endpoint.port, 9000);
    EXPECT_EQ(authority(p->endpoint), "minio.internal:9000");
    EXPECT_EQ(p->region, "us-east-1");
    EXPECT_EQ(p->addressing, Addressing::PathStyle);
    EXPECT_TRUE(p->supports_conditional_put);
    EXPECT_EQ(p->min_part_bytes, 5U * 1024 * 1024);
    EXPECT_EQ(p->max_part_bytes, 5ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(p->max_parts, 10'000U);
    EXPECT_EQ(p->max_presign_ttl, std::chrono::hours(7 * 24));
    EXPECT_FALSE(p->uniform_parts_required);
}

TEST(S3Profile, R2SignsForRegionAutoAndRequiresUniformParts) {
    const auto p = S3Profile::r2("0123456789abcdef0123456789abcdef");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(authority(p->endpoint), "0123456789abcdef0123456789abcdef.r2.cloudflarestorage.com");
    EXPECT_EQ(p->endpoint.scheme, Scheme::Https);
    EXPECT_EQ(p->region, "auto");
    EXPECT_EQ(p->addressing, Addressing::PathStyle);
    EXPECT_TRUE(p->uniform_parts_required);
}

TEST(S3Profile, RejectsAccountIdsThatAreNotThirtyTwoLowercaseHexDigits) {
    for (const std::string_view a :
         {"", "0123456789abcdef0123456789abcde", "0123456789abcdef0123456789abcdef0",
          "0123456789ABCDEF0123456789ABCDEF", "0123456789abcdef0123456789abcdeg",
          "0123456789abcdef.123456789abcdef", "evil.example/0123456789abcdef0123"}) {
        EXPECT_EQ(S3Profile::r2(a).error_or(ProfileError::InvalidEndpoint),
                  ProfileError::InvalidAccountId)
            << a;
    }
}

TEST(Endpoint, LeavesTheSchemeDefaultPortOutOfTheAuthority) {
    const auto https = parse_endpoint("https://storage.example:443");
    ASSERT_TRUE(https.has_value());
    EXPECT_EQ(https->port, 443);
    EXPECT_EQ(authority(*https), "storage.example");

    const auto http = parse_endpoint("http://127.0.0.1");
    ASSERT_TRUE(http.has_value());
    EXPECT_EQ(http->port, 80);
    EXPECT_EQ(authority(*http), "127.0.0.1");

    const auto http_on_443 = parse_endpoint("http://storage.example:443");
    ASSERT_TRUE(http_on_443.has_value());
    EXPECT_EQ(authority(*http_on_443), "storage.example:443");
}

TEST(Endpoint, RejectsAnythingButSchemeHostAndPort) {
    for (const std::string_view e : {"",
                                     "minio:9000",
                                     "ftp://minio",
                                     "://minio",
                                     "httpss://minio",
                                     " http://minio",
                                     "https:/minio",
                                     "http://",
                                     "https://:9000",
                                     "http://minio:",
                                     "http://minio:0",
                                     "http://minio:65536",
                                     "http://minio:+80",
                                     "http://minio:9000/",
                                     "http://minio/bucket",
                                     "http://user@minio",
                                     "http://minio?x=1",
                                     "http://[::1]:9000",
                                     "HTTP://minio",
                                     "http://Minio",
                                     "http://minio..internal",
                                     "http://.minio",
                                     "http://minio.",
                                     "http://mi nio"}) {
        EXPECT_EQ(parse_endpoint(e).error_or(ProfileError::InvalidAccountId),
                  ProfileError::InvalidEndpoint)
            << e;
        EXPECT_FALSE(S3Profile::minio(e).has_value()) << e;
    }
}

} // namespace
