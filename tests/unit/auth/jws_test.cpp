#include "core/ports/auth.hpp"
#include "infra/auth/base64url.hpp"

#include "jws.hpp"

#include <cstddef>
#include <gtest/gtest.h>
#include <string>
#include <string_view>

namespace {

using core::ports::AuthError;
using infra::auth::encode_base64url;
using infra::auth::detail::Algorithm;
using infra::auth::detail::kMaxTokenBytes;
using infra::auth::detail::parse_compact;

const std::string kPayload = encode_base64url(std::string_view{R"({"sub":"alice"})"});
const std::string kSignature = encode_base64url(std::string_view{"signature bytes"});

std::string with_header(std::string_view header_json) {
    return encode_base64url(header_json) + '.' + kPayload + '.' + kSignature;
}

AuthError error_of(std::string_view token) {
    const auto parsed = parse_compact(token);
    EXPECT_FALSE(parsed.has_value()) << token;
    return parsed ? AuthError::Malformed : parsed.error();
}

TEST(CompactJwsTest, SplitsAWellFormedToken) {
    const std::string header = encode_base64url(std::string_view{R"({"alg":"ES256","kid":"k1"})"});
    const std::string token = header + '.' + kPayload + '.' + kSignature;
    const auto jws = parse_compact(token);
    ASSERT_TRUE(jws.has_value());
    EXPECT_EQ(jws->alg, Algorithm::ES256);
    EXPECT_EQ(jws->kid, "k1");
    EXPECT_EQ(jws->signing_input, header + '.' + kPayload);
    EXPECT_EQ(jws->payload, R"({"sub":"alice"})");
    EXPECT_EQ(jws->signature, "signature bytes");
}

TEST(CompactJwsTest, RequiresExactlyTwoDots) {
    const std::string good = with_header(R"({"alg":"EdDSA","kid":"k1"})");
    ASSERT_TRUE(parse_compact(good).has_value());
    EXPECT_EQ(error_of(good + ".extra"), AuthError::Malformed);
    EXPECT_EQ(error_of(good.substr(0, good.rfind('.'))), AuthError::Malformed);
    EXPECT_EQ(error_of("no-dots-at-all"), AuthError::Malformed);
    EXPECT_EQ(error_of(".."), AuthError::Malformed);
    EXPECT_EQ(error_of("..."), AuthError::Malformed);
}

TEST(CompactJwsTest, RejectsEmptySegments) {
    const std::string header = encode_base64url(std::string_view{R"({"alg":"EdDSA","kid":"k1"})"});
    EXPECT_EQ(error_of(header + ".." + kSignature), AuthError::Malformed);
    EXPECT_EQ(error_of(header + '.' + kPayload + '.'), AuthError::Malformed);
    EXPECT_EQ(error_of('.' + kPayload + '.' + kSignature), AuthError::Malformed);
}

TEST(CompactJwsTest, RejectsAlgNoneWithOrWithoutASignature) {
    const std::string header = encode_base64url(std::string_view{R"({"alg":"none","kid":"k1"})"});
    EXPECT_EQ(error_of(header + '.' + kPayload + '.'), AuthError::UnsupportedAlgorithm);
    EXPECT_EQ(error_of(header + '.' + kPayload + '.' + kSignature),
              AuthError::UnsupportedAlgorithm);
}

TEST(CompactJwsTest, RejectsAlgorithmsOutsideTheSupportedFour) {
    for (const std::string_view alg : {"HS256", "RS512", "ES384", "rs256", "EdDSA ", ""}) {
        const std::string header =
            std::string(R"({"alg":")") + std::string(alg) + R"(","kid":"k"})";
        EXPECT_EQ(error_of(with_header(header)), AuthError::UnsupportedAlgorithm) << alg;
    }
    EXPECT_EQ(error_of(with_header(R"({"alg":5,"kid":"k"})")), AuthError::Malformed);
    EXPECT_EQ(error_of(with_header(R"({"kid":"k"})")), AuthError::Malformed);
}

TEST(CompactJwsTest, RejectsCriticalExtensions) {
    EXPECT_EQ(error_of(with_header(R"({"alg":"EdDSA","kid":"k","crit":["b64"],"b64":false})")),
              AuthError::Malformed);
}

TEST(CompactJwsTest, RequiresABoundedStringKid) {
    EXPECT_EQ(error_of(with_header(R"({"alg":"EdDSA"})")), AuthError::Malformed);
    EXPECT_EQ(error_of(with_header(R"({"alg":"EdDSA","kid":""})")), AuthError::Malformed);
    EXPECT_EQ(error_of(with_header(R"({"alg":"EdDSA","kid":7})")), AuthError::Malformed);
    const std::string long_kid(infra::auth::detail::kMaxKidBytes + 1, 'k');
    EXPECT_EQ(error_of(with_header(R"({"alg":"EdDSA","kid":")" + long_kid + R"("})")),
              AuthError::Malformed);
}

TEST(CompactJwsTest, RejectsHeadersThatAreNotAUniqueKeyedObject) {
    EXPECT_EQ(error_of(with_header(R"(["alg","EdDSA"])")), AuthError::Malformed);
    EXPECT_EQ(error_of(with_header(R"({"alg":"EdDSA","kid":"k")")), AuthError::Malformed);
    // A second alg that a laxer parser would let win.
    EXPECT_EQ(error_of(with_header(R"({"alg":"EdDSA","kid":"k","alg":"none"})")),
              AuthError::Malformed);
}

TEST(CompactJwsTest, RejectsSegmentsThatAreNotStrictBase64Url) {
    const std::string header = encode_base64url(std::string_view{R"({"alg":"EdDSA","kid":"k"})"});
    EXPECT_EQ(error_of(header + "=." + kPayload + '.' + kSignature), AuthError::Malformed);
    EXPECT_EQ(error_of(header + '.' + kPayload + "+." + kSignature), AuthError::Malformed);
    EXPECT_EQ(error_of(header + '.' + kPayload + '.' + kSignature + "=="), AuthError::Malformed);
    // "signature bytes" is 15 bytes, so its last character carries no spare bits; one more
    // byte leaves spare bits, and setting them must not yield a second valid spelling.
    const std::string sig = encode_base64url(std::string_view{"16 byte sig here"});
    ASSERT_EQ(sig.back(), 'Q');
    ASSERT_TRUE(parse_compact(header + '.' + kPayload + '.' + sig).has_value());
    EXPECT_EQ(error_of(header + '.' + kPayload + '.' + sig.substr(0, sig.size() - 1) + 'R'),
              AuthError::Malformed);
}

TEST(CompactJwsTest, AcceptsTokensUpToTheSizeLimitAndNoLonger) {
    const std::string head =
        encode_base64url(std::string_view{R"({"alg":"EdDSA","kid":"k"})"}) + '.' + kPayload + '.';
    // Runs of 'A' decode to zero bytes, except a run of length 1 mod 4, which cannot decode.
    const auto signature_of = [](std::size_t len) {
        return std::string(len % 4 == 1 ? len - 1 : len, 'A');
    };
    const std::string at_limit = head + signature_of(kMaxTokenBytes - head.size());
    ASSERT_LE(at_limit.size(), kMaxTokenBytes);
    ASSERT_GE(at_limit.size(), kMaxTokenBytes - 1);
    EXPECT_TRUE(parse_compact(at_limit).has_value());
    const std::string over = head + signature_of(kMaxTokenBytes - head.size() + 2);
    ASSERT_GT(over.size(), kMaxTokenBytes);
    EXPECT_EQ(error_of(over), AuthError::Malformed);
    EXPECT_EQ(error_of(""), AuthError::Malformed);
}

} // namespace
