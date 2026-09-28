#include "core/util/time.hpp"
#include "infra/s3util/credentials.hpp"
#include "infra/s3util/crypto.hpp"
#include "infra/s3util/profile.hpp"
#include "infra/s3util/sigv4.hpp"
#include "infra/s3util/url.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <gtest/gtest.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using infra::s3util::AmzDate;
using infra::s3util::canonical_request;
using infra::s3util::Credentials;
using infra::s3util::derive_signing_key;
using infra::s3util::Header;
using infra::s3util::hmac_sha256;
using infra::s3util::kEmptyPayloadSha256;
using infra::s3util::kUnsignedPayload;
using infra::s3util::payload_sha256;
using infra::s3util::PresignError;
using infra::s3util::QueryParam;
using infra::s3util::RequestTarget;
using infra::s3util::Scheme;
using infra::s3util::SecretString;
using infra::s3util::sha256;
using infra::s3util::Signer;
using infra::s3util::to_hex;

// Every expected value below is from the SigV4 examples in the S3 API reference ("Signature
// Calculations for the Authorization Header" and "Query String Authentication"), or was
// computed from the same inputs with an independent implementation where AWS publishes none.
constexpr std::string_view kAccessKeyId = "AKIAIOSFODNN7EXAMPLE";
constexpr std::string_view kSecret = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
constexpr std::string_view kExampleHost = "examplebucket.s3.amazonaws.com";
constexpr std::string_view kToken = "FwoGZXIvYXdzE+/abc==";

constexpr core::WallTime utc(int y, unsigned m, unsigned d,
                             std::chrono::milliseconds time_of_day = {}) {
    const std::chrono::year_month_day date{std::chrono::year{y}, std::chrono::month{m},
                                           std::chrono::day{d}};
    return core::WallTime{std::chrono::sys_days{date}} + time_of_day;
}

constexpr core::WallTime kExampleTime = utc(2013, 5, 24);

Credentials example_credentials(std::string_view secret = kSecret) {
    return Credentials::make(kAccessKeyId, SecretString(secret)).value();
}

Credentials credentials_with_token() {
    return Credentials::make(kAccessKeyId, SecretString(kSecret), SecretString(kToken)).value();
}

RequestTarget example_target(std::string path = "/test.txt", std::vector<QueryParam> query = {}) {
    return RequestTarget{.scheme = Scheme::Https,
                         .host = std::string(kExampleHost),
                         .path = std::move(path),
                         .query = std::move(query)};
}

std::optional<std::string> header(const std::vector<Header>& headers, std::string_view name) {
    for (const auto& h : headers) {
        if (h.name == name) {
            return h.value;
        }
    }
    return std::nullopt;
}

std::string signature_of(const std::vector<Header>& headers) {
    const std::string authorization = header(headers, "authorization").value_or("");
    const std::size_t at = authorization.rfind("Signature=");
    return at == std::string::npos ? "" : authorization.substr(at + 10);
}

std::string sign_example(const Signer& signer, core::WallTime when,
                         const Credentials& credentials) {
    return signature_of(
        signer.sign("GET", example_target(), {}, kEmptyPayloadSha256, credentials, when));
}

TEST(AmzDate, FormatsUtcToTheSecond) {
    const AmzDate d(kExampleTime);
    EXPECT_EQ(d.datetime(), "20130524T000000Z");
    EXPECT_EQ(d.date(), "20130524");

    using std::chrono::hours, std::chrono::minutes, std::chrono::seconds;
    EXPECT_EQ(AmzDate(utc(2024, 2, 29, hours(13) + minutes(7) + seconds(9))).datetime(),
              "20240229T130709Z");
}

TEST(AmzDate, TruncatesSubsecondsRatherThanRoundingIntoTheNextDay) {
    using std::chrono::hours, std::chrono::minutes, std::chrono::seconds;
    const auto last_moment = hours(23) + minutes(59) + seconds(59) + std::chrono::milliseconds(999);
    EXPECT_EQ(AmzDate(utc(2026, 1, 1, last_moment)).datetime(), "20260101T235959Z");
}

TEST(AmzDate, FloorsInstantsBeforeTheEpoch) {
    EXPECT_EQ(AmzDate(core::WallTime{} - std::chrono::seconds(1)).datetime(), "19691231T235959Z");
}

TEST(Sha256, MatchesKnownDigests) {
    EXPECT_EQ(to_hex(sha256(std::string_view{})), kEmptyPayloadSha256);
    const std::string_view body = "Welcome to Amazon S3.";
    EXPECT_EQ(payload_sha256(std::as_bytes(std::span(body))),
              "44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072");
}

TEST(SigV4, CanonicalRequestMatchesTheAwsGetObjectExample) {
    const std::array headers{
        Header{.name = "host", .value = std::string(kExampleHost)},
        Header{.name = "x-amz-content-sha256", .value = std::string(kEmptyPayloadSha256)},
        Header{.name = "x-amz-date", .value = "20130524T000000Z"}};
    const std::string canonical =
        canonical_request("GET", example_target(), headers, kEmptyPayloadSha256);
    EXPECT_EQ(canonical, "GET\n"
                         "/test.txt\n"
                         "\n"
                         "host:examplebucket.s3.amazonaws.com\n"
                         "x-amz-content-sha256:"
                         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n"
                         "x-amz-date:20130524T000000Z\n"
                         "\n"
                         "host;x-amz-content-sha256;x-amz-date\n"
                         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(to_hex(sha256(canonical)),
              "e155673fa5bcd4b855a77a15b98fce3d10f286f93a203d6d98d2eb51f885f9b7");
}

TEST(SigV4, KeyChainMatchesTheAwsExampleAtEveryStep) {
    std::vector<unsigned char> seed;
    for (const char c : "AWS4" + std::string(kSecret)) {
        seed.push_back(static_cast<unsigned char>(c));
    }
    const auto k_date = hmac_sha256(seed, "20130524");
    EXPECT_EQ(to_hex(k_date), "68896419206d6240ad4cd7dc8ba658efbf3b43b53041950083a10833824fcfbb");
    const auto k_region = hmac_sha256(k_date, "us-east-1");
    EXPECT_EQ(to_hex(k_region), "0506335cc36b4a971f6beddf0adbd976ee71222cb42c131487e0c12c5c47a025");
    const auto k_service = hmac_sha256(k_region, "s3");
    EXPECT_EQ(to_hex(k_service),
              "05602c14e8b6aad30e7f6dec4b544071f6e4a742934bc5e36415733c47a67d44");
    const auto k_signing = hmac_sha256(k_service, "aws4_request");
    EXPECT_EQ(to_hex(k_signing),
              "dbb893acc010964918f1fd433add87c70e8b0db6be30c1fbeafefa5ec6ba8378");

    EXPECT_EQ(to_hex(derive_signing_key(SecretString(kSecret), AmzDate(kExampleTime), "us-east-1")),
              "dbb893acc010964918f1fd433add87c70e8b0db6be30c1fbeafefa5ec6ba8378");

    const std::string to_sign =
        "AWS4-HMAC-SHA256\n20130524T000000Z\n20130524/us-east-1/s3/aws4_request\n"
        "e155673fa5bcd4b855a77a15b98fce3d10f286f93a203d6d98d2eb51f885f9b7";
    EXPECT_EQ(to_hex(hmac_sha256(k_signing, to_sign)),
              "df548e2ce037944d03f3e68682813b093763996d597cf890ca3d9037fd231eb4");
}

TEST(SigV4, SignerReproducesTheAwsGetObjectAuthorizationHeader) {
    const Signer signer("us-east-1");
    const auto headers = signer.sign("GET", example_target(), {}, kEmptyPayloadSha256,
                                     example_credentials(), kExampleTime);

    ASSERT_EQ(headers.size(), 4U);
    EXPECT_EQ(headers[0].name, "host");
    EXPECT_EQ(headers[0].value, kExampleHost);
    EXPECT_EQ(headers[1].name, "x-amz-content-sha256");
    EXPECT_EQ(headers[1].value, kEmptyPayloadSha256);
    EXPECT_EQ(headers[2].name, "x-amz-date");
    EXPECT_EQ(headers[2].value, "20130524T000000Z");
    EXPECT_EQ(headers[3].name, "authorization");
    EXPECT_EQ(headers[3].value,
              "AWS4-HMAC-SHA256 "
              "Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, "
              "SignedHeaders=host;x-amz-content-sha256;x-amz-date, "
              "Signature=df548e2ce037944d03f3e68682813b093763996d597cf890ca3d9037fd231eb4");
}

TEST(SigV4, SignerReproducesTheOtherAwsHeaderExamples) {
    const Signer signer("us-east-1");
    const auto creds = example_credentials();

    const std::string_view body = "Welcome to Amazon S3.";
    const std::array put_headers{
        Header{.name = "Date", .value = "Fri, 24 May 2013 00:00:00 GMT"},
        Header{.name = "x-amz-storage-class", .value = "REDUCED_REDUNDANCY"}};
    const auto put =
        signer.sign("PUT", example_target("/test$file.text"), put_headers,
                    payload_sha256(std::as_bytes(std::span(body))), creds, kExampleTime);
    EXPECT_EQ(signature_of(put),
              "98ad721746da40c64f1a55b78f14c238d841ea1380cd77a1b5971af0ece108bd");

    const std::array range{Header{.name = "Range", .value = "bytes=0-9"}};
    EXPECT_EQ(signature_of(signer.sign("GET", example_target(), range, kEmptyPayloadSha256, creds,
                                       kExampleTime)),
              "f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");

    EXPECT_EQ(
        signature_of(signer.sign("GET", example_target("/", {{.name = "lifecycle", .value = ""}}),
                                 {}, kEmptyPayloadSha256, creds, kExampleTime)),
        "fea454ca298b7da1c68078a5d1bdbfbbe0d65c699e0f91ac7a200a0136783543");

    EXPECT_EQ(signature_of(signer.sign("GET",
                                       example_target("/", {{.name = "prefix", .value = "J"},
                                                            {.name = "max-keys", .value = "2"}}),
                                       {}, kEmptyPayloadSha256, creds, kExampleTime)),
              "34b48302e7b5fa45bde8084f4b7868a86f0a534bc59db6670ed5711ef69dc6f7");
}

TEST(SigV4, CanonicalHeadersAreLoweredTrimmedCollapsedSortedAndFolded) {
    const std::array headers{Header{.name = "X-Amz-Meta-Tag", .value = "  a   b\t\t c  "},
                             Header{.name = "Host", .value = "h"},
                             Header{.name = "x-amz-meta-tag", .value = "d"},
                             Header{.name = "Content-Type", .value = "video/mp4"}};
    const RequestTarget target{.scheme = Scheme::Https, .host = "h", .path = "/k", .query = {}};
    EXPECT_EQ(canonical_request("PUT", target, headers, kUnsignedPayload),
              "PUT\n/k\n\n"
              "content-type:video/mp4\n"
              "host:h\n"
              "x-amz-meta-tag:a b c,d\n"
              "\n"
              "content-type;host;x-amz-meta-tag\n"
              "UNSIGNED-PAYLOAD");
}

TEST(SigV4, SignsEveryHeaderItReturns) {
    const Signer signer("us-east-1");
    const std::array extra{Header{.name = "X-Amz-Meta-Owner", .value = "ulw"},
                           Header{.name = "Content-Type", .value = "video/mp4"},
                           Header{.name = "If-None-Match", .value = "*"}};
    const auto headers = signer.sign("PUT", example_target("/raw/v1"), extra, kUnsignedPayload,
                                     example_credentials(), kExampleTime);

    std::string names;
    for (const auto& h : headers) {
        if (h.name != "authorization") {
            names.append(names.empty() ? "" : ";").append(h.name);
        }
    }
    EXPECT_EQ(names, "content-type;host;if-none-match;x-amz-content-sha256;x-amz-date;"
                     "x-amz-meta-owner");
    EXPECT_NE(header(headers, "authorization").value_or("").find("SignedHeaders=" + names + ","),
              std::string::npos);
    EXPECT_EQ(header(headers, "x-amz-content-sha256"), "UNSIGNED-PAYLOAD");
}

TEST(SigV4, SessionTokenIsSentAndSigned) {
    const Signer signer("us-east-1");
    const auto headers = signer.sign("GET", example_target(), {}, kEmptyPayloadSha256,
                                     credentials_with_token(), kExampleTime);
    EXPECT_EQ(header(headers, "x-amz-security-token"), kToken);
    EXPECT_NE(header(headers, "authorization")
                  .value_or("")
                  .find("SignedHeaders=host;x-amz-content-sha256;x-amz-date;x-amz-security-token,"),
              std::string::npos);
    EXPECT_EQ(signature_of(headers),
              "6383fe7a70cab072781583222cc50384b27bc96cdbc2fb9662a3e1ec63e4abb8");
}

TEST(Presign, ReproducesTheAwsQueryStringExample) {
    const Signer signer("us-east-1");
    const auto url = signer.presign("GET", example_target(), std::chrono::hours(24),
                                    example_credentials(), kExampleTime);
    ASSERT_TRUE(url.has_value());
    EXPECT_EQ(*url,
              "https://examplebucket.s3.amazonaws.com/test.txt"
              "?X-Amz-Algorithm=AWS4-HMAC-SHA256"
              "&X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20130524%2Fus-east-1%2Fs3%2Faws4_request"
              "&X-Amz-Date=20130524T000000Z"
              "&X-Amz-Expires=86400"
              "&X-Amz-SignedHeaders=host"
              "&X-Amz-Signature="
              "aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957d157751f604d404");
}

TEST(Presign, SignsTheSessionTokenAndTheCallersOwnParameters) {
    const Signer with_token("us-east-1");
    EXPECT_EQ(with_token
                  .presign("GET", example_target(), std::chrono::hours(1), credentials_with_token(),
                           kExampleTime)
                  .value_or(""),
              "https://examplebucket.s3.amazonaws.com/test.txt"
              "?X-Amz-Algorithm=AWS4-HMAC-SHA256"
              "&X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20130524%2Fus-east-1%2Fs3%2Faws4_request"
              "&X-Amz-Date=20130524T000000Z"
              "&X-Amz-Expires=3600"
              "&X-Amz-Security-Token=FwoGZXIvYXdzE%2B%2Fabc%3D%3D"
              "&X-Amz-SignedHeaders=host"
              "&X-Amz-Signature=58fbd994203548d8e37666c3a5dc4e9cacdcfde6a918ce73010b613299cbd743");

    const Signer r2("auto");
    const RequestTarget playlist{
        .scheme = Scheme::Https,
        .host = "0123456789abcdef0123456789abcdef.r2.cloudflarestorage.com",
        .path = "/media/hls/v1/master.m3u8",
        .query = {{.name = "response-content-type", .value = "application/vnd.apple.mpegurl"}}};
    EXPECT_EQ(r2.presign("GET", playlist, std::chrono::minutes(15), example_credentials(),
                         utc(2026, 1, 1))
                  .value_or(""),
              "https://0123456789abcdef0123456789abcdef.r2.cloudflarestorage.com"
              "/media/hls/v1/master.m3u8"
              "?X-Amz-Algorithm=AWS4-HMAC-SHA256"
              "&X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20260101%2Fauto%2Fs3%2Faws4_request"
              "&X-Amz-Date=20260101T000000Z"
              "&X-Amz-Expires=900"
              "&X-Amz-SignedHeaders=host"
              "&response-content-type=application%2Fvnd.apple.mpegurl"
              "&X-Amz-Signature=4cc16571e63e8a37b769af6f8fcae0e40a5b7723cfa8b58b59bebb41ac3a0d99");
}

TEST(Presign, AcceptsOnlyExpiriesFromOneSecondToSevenDays) {
    const Signer signer("us-east-1");
    const auto creds = example_credentials();
    const auto presign = [&](std::chrono::seconds expires) {
        return signer.presign("GET", example_target(), expires, creds, kExampleTime);
    };
    EXPECT_EQ(presign(std::chrono::seconds(0)), std::unexpected(PresignError::ExpiryOutOfRange));
    EXPECT_EQ(presign(std::chrono::seconds(-1)), std::unexpected(PresignError::ExpiryOutOfRange));
    EXPECT_EQ(presign(std::chrono::seconds(604'801)),
              std::unexpected(PresignError::ExpiryOutOfRange));
    EXPECT_NE(presign(std::chrono::seconds(1)).value_or("").find("X-Amz-Expires=1&"),
              std::string::npos);
    EXPECT_NE(presign(std::chrono::seconds(604'800)).value_or("").find("X-Amz-Expires=604800&"),
              std::string::npos);
}

TEST(SigningKeyCache, DerivesAFreshKeyWhenTheDateChanges) {
    const Signer signer("us-east-1");
    const auto creds = example_credentials();
    EXPECT_EQ(sign_example(signer, kExampleTime, creds),
              "df548e2ce037944d03f3e68682813b093763996d597cf890ca3d9037fd231eb4");
    EXPECT_EQ(sign_example(signer, utc(2013, 5, 25), creds),
              "34e37fbebb1a8127e4d28b82fa70120d81581aa492bd956787ee1e5149a6d09b");
    EXPECT_EQ(sign_example(signer, kExampleTime, creds),
              "df548e2ce037944d03f3e68682813b093763996d597cf890ca3d9037fd231eb4");
}

TEST(SigningKeyCache, DoesNotServeAKeyDerivedFromARotatedOutSecret) {
    const Signer signer("us-east-1");
    EXPECT_EQ(sign_example(signer, kExampleTime, example_credentials()),
              "df548e2ce037944d03f3e68682813b093763996d597cf890ca3d9037fd231eb4");
    EXPECT_EQ(sign_example(signer, kExampleTime, example_credentials("rotated-secret")),
              "f4a15d42c2e0fe76ba2973431f3d4581daddadaa82705b079c8b5cab813070e8");
}

TEST(SigningKeyCache, StaysCorrectAfterEvictingEntries) {
    const Signer shared("us-east-1");
    const auto creds = example_credentials();
    for (unsigned day = 1; day <= 8; ++day) {
        const auto when = utc(2013, 5, day);
        EXPECT_EQ(sign_example(shared, when, creds), sign_example(Signer("us-east-1"), when, creds))
            << day;
    }
    EXPECT_EQ(sign_example(shared, utc(2013, 5, 1), creds),
              sign_example(Signer("us-east-1"), utc(2013, 5, 1), creds));
}

TEST(SigningKeyCache, ConcurrentSignersAgreeWithASerialSigner) {
    const std::array days{utc(2013, 5, 24), utc(2013, 5, 25), utc(2013, 5, 26)};
    const std::array secrets{std::string_view(kSecret), std::string_view("rotated-secret")};
    std::vector<std::string> expected;
    for (const auto day : days) {
        for (const auto secret : secrets) {
            expected.push_back(sign_example(Signer("us-east-1"), day, example_credentials(secret)));
        }
    }

    const Signer shared("us-east-1");
    constexpr int kThreads = 4;
    constexpr int kRounds = 50;
    std::array<int, kThreads> mismatches{};
    {
        std::vector<std::jthread> threads;
        threads.reserve(kThreads);
        for (std::size_t t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                for (int round = 0; round < kRounds; ++round) {
                    const std::size_t i = (t + static_cast<std::size_t>(round)) % expected.size();
                    const auto creds = example_credentials(secrets.at(i % secrets.size()));
                    if (sign_example(shared, days.at(i / secrets.size()), creds) !=
                        expected.at(i)) {
                        ++mismatches.at(t);
                    }
                }
            });
        }
    }
    EXPECT_EQ(mismatches, (std::array<int, kThreads>{}));
}

} // namespace
