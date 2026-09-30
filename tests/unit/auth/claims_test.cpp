#include "core/ports/auth.hpp"
#include "core/util/time.hpp"
#include "infra/auth/base64url.hpp"
#include "infra/auth/claim_rules.hpp"

#include "claims.hpp"
#include "jwk.hpp"
#include "jws.hpp"
#include "support/fake_clock.hpp"
#include "test_claims.hpp"
#include "test_keys.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <gtest/gtest.h>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using core::ports::AuthError;
using infra::auth::ClaimRules;
using infra::auth::detail::check_claims;

using ulw::test::numeric_date;
using ulw::test::test_payload;

const ClaimRules& kRules = ulw::test::kTestRules;

class ClaimsTest : public ::testing::Test {
protected:
    [[nodiscard]] core::ports::VerifyResult check(const std::string& json) const {
        return check_claims(json, kRules, clock_.wall_now());
    }
    [[nodiscard]] AuthError error_of(const std::string& json) const {
        const auto result = check(json);
        EXPECT_FALSE(result.has_value()) << json;
        return result ? AuthError::Malformed : result.error();
    }

    ulw::test::FakeClock clock_;
};

TEST_F(ClaimsTest, ATokenThatMeetsEveryRuleYieldsItsClaims) {
    const auto claims = check(test_payload());
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->subject.view(), "alice");
    EXPECT_EQ(claims->email, "alice@example.com");
    EXPECT_EQ(claims->expires_at, clock_.wall_now() + std::chrono::hours(1));
}

TEST_F(ClaimsTest, TheIssuerMustMatchExactly) {
    EXPECT_EQ(error_of(test_payload({{"iss", R"("https://id.askedin.test/")"}})),
              AuthError::WrongIssuer);
    EXPECT_EQ(error_of(test_payload({{"iss", R"("https://evil.test")"}})), AuthError::WrongIssuer);
    EXPECT_EQ(error_of(test_payload({{"iss", std::nullopt}})), AuthError::WrongIssuer);
    EXPECT_EQ(error_of(test_payload({{"iss", "7"}})), AuthError::WrongIssuer);
}

TEST_F(ClaimsTest, TheAudienceMayBeOneStringOrAnArrayHoldingOurs) {
    EXPECT_TRUE(check(test_payload({{"aud", R"(["other","askedin-platform"])"}})).has_value());
    EXPECT_EQ(error_of(test_payload({{"aud", R"("askedin-platform-stage")"}})),
              AuthError::WrongAudience);
    EXPECT_EQ(error_of(test_payload({{"aud", R"(["other","another"])"}})),
              AuthError::WrongAudience);
    EXPECT_EQ(error_of(test_payload({{"aud", "[]"}})), AuthError::WrongAudience);
    EXPECT_EQ(error_of(test_payload({{"aud", std::nullopt}})), AuthError::WrongAudience);
    EXPECT_EQ(error_of(test_payload({{"aud", R"(["askedin-platform",7])"}})), AuthError::Malformed);
    EXPECT_EQ(error_of(test_payload({{"aud", R"({"aud":"askedin-platform"})"}})),
              AuthError::Malformed);
}

TEST_F(ClaimsTest, ExpiryAllowsTheSkewAndNotOneSecondMore) {
    EXPECT_TRUE(check(test_payload({{"exp", numeric_date(-59)}})).has_value());
    EXPECT_EQ(error_of(test_payload({{"exp", numeric_date(-60)}})), AuthError::Expired);
    EXPECT_EQ(error_of(test_payload({{"exp", numeric_date(-86400)}})), AuthError::Expired);
}

TEST_F(ClaimsTest, NotBeforeAllowsTheSkewAndNotOneSecondMore) {
    EXPECT_TRUE(check(test_payload({{"nbf", numeric_date(60)}})).has_value());
    EXPECT_EQ(error_of(test_payload({{"nbf", numeric_date(61)}})), AuthError::NotYetValid);
    EXPECT_TRUE(check(test_payload({{"nbf", numeric_date(-3600)}})).has_value());
}

// Askedin's contract asks for exp and nbf only (docs/integration/auth.md), so issued-at is
// never consulted: a token minted by a skewed issuer clock still verifies while nbf/exp allow.
TEST_F(ClaimsTest, IssuedAtIsNotConsultedWhateverItSays) {
    EXPECT_TRUE(check(test_payload({{"iat", numeric_date(30)}})).has_value());
    EXPECT_TRUE(check(test_payload({{"iat", numeric_date(61)}})).has_value());
    EXPECT_TRUE(check(test_payload({{"iat", numeric_date(86400)}})).has_value());
    EXPECT_TRUE(check(test_payload({{"iat", R"("yesterday")"}})).has_value());
    EXPECT_EQ(error_of(test_payload({{"iat", numeric_date(-60)}, {"exp", numeric_date(-60)}})),
              AuthError::Expired);
}

TEST_F(ClaimsTest, ExpiryIsRequiredAndMustBeAnIntegerTheClockCanHold) {
    EXPECT_EQ(error_of(test_payload({{"exp", std::nullopt}})), AuthError::Malformed);
    EXPECT_EQ(error_of(test_payload({{"exp", '"' + numeric_date(3600) + '"'}})),
              AuthError::Malformed);
    EXPECT_EQ(error_of(test_payload({{"exp", numeric_date(3600) + ".5"}})), AuthError::Malformed);
    EXPECT_EQ(error_of(test_payload({{"exp", "1e10"}})), AuthError::Malformed);
    // Past what a nanosecond system_clock can represent (year 2262).
    EXPECT_EQ(error_of(test_payload({{"exp", "9300000000"}})), AuthError::Malformed);
    EXPECT_EQ(error_of(test_payload({{"exp", "18446744073709551615"}})), AuthError::Malformed);
    // Before what it can represent (year 1677), which would overflow on the way to a time point.
    EXPECT_EQ(error_of(test_payload({{"nbf", "-9300000000"}})), AuthError::Malformed);
    EXPECT_EQ(error_of(test_payload({{"nbf", R"("soon")"}})), AuthError::Malformed);
}

TEST_F(ClaimsTest, TheSubjectFallsBackToAskedinsIdClaim) {
    const auto by_string = check(test_payload({{"sub", std::nullopt}, {"id", R"("u-42")"}}));
    ASSERT_TRUE(by_string.has_value());
    EXPECT_EQ(by_string->subject.view(), "u-42");
    const auto by_number =
        check(test_payload({{"sub", std::nullopt}, {"id", "18446744073709551615"}}));
    ASSERT_TRUE(by_number.has_value());
    EXPECT_EQ(by_number->subject.view(), "18446744073709551615");
    const auto sub_wins = check(test_payload({{"id", R"("u-42")"}}));
    ASSERT_TRUE(sub_wins.has_value());
    EXPECT_EQ(sub_wins->subject.view(), "alice");
}

TEST_F(ClaimsTest, AnAbsentOrEmptySubjectIsMissing) {
    EXPECT_EQ(error_of(test_payload({{"sub", std::nullopt}})), AuthError::MissingSubject);
    EXPECT_EQ(error_of(test_payload({{"sub", R"("")"}})), AuthError::MissingSubject);
    EXPECT_EQ(error_of(test_payload({{"sub", std::nullopt}, {"id", R"("")"}})),
              AuthError::MissingSubject);
}

TEST_F(ClaimsTest, ASubjectOutsideTheUserIdFormIsMalformed) {
    EXPECT_EQ(error_of(test_payload({{"sub", R"("alice smith")"}})), AuthError::Malformed);
    EXPECT_EQ(error_of(test_payload({{"sub", R"("alice\nsmith")"}})), AuthError::Malformed);
    EXPECT_EQ(error_of(test_payload({{"sub", '"' + std::string(129, 'a') + '"'}})),
              AuthError::Malformed);
    EXPECT_EQ(error_of(test_payload({{"sub", "42"}})), AuthError::Malformed);
    EXPECT_EQ(error_of(test_payload({{"sub", std::nullopt}, {"id", "-1"}})), AuthError::Malformed);
    EXPECT_TRUE(check(test_payload({{"sub", '"' + std::string(128, 'a') + '"'}})).has_value());
}

TEST_F(ClaimsTest, EmailIsOptionalButMustBeAPrintableString) {
    const auto absent = check(test_payload({{"email", std::nullopt}}));
    ASSERT_TRUE(absent.has_value());
    EXPECT_EQ(absent->email, "");
    const auto null = check(test_payload({{"email", "null"}}));
    ASSERT_TRUE(null.has_value());
    EXPECT_EQ(null->email, "");
    EXPECT_EQ(error_of(test_payload({{"email", "7"}})), AuthError::Malformed);
    EXPECT_EQ(error_of(test_payload({{"email", R"("a@b.test\r\nX-Injected: 1")"}})),
              AuthError::Malformed);
    EXPECT_EQ(error_of(test_payload({{"email", R"("a@b.test\u007f")"}})), AuthError::Malformed);
    EXPECT_EQ(error_of(test_payload({{"email", '"' + std::string(255, 'a') + '"'}})),
              AuthError::Malformed);
}

TEST_F(ClaimsTest, APayloadMustBeAnObjectWithoutRepeatedClaims) {
    EXPECT_EQ(error_of("[]"), AuthError::Malformed);
    EXPECT_EQ(error_of("not json"), AuthError::Malformed);
    std::string repeated = test_payload();
    repeated.insert(repeated.size() - 1, R"(,"sub":"mallory")");
    EXPECT_EQ(error_of(repeated), AuthError::Malformed);
}

TEST_F(ClaimsTest, TheIssuerIsJudgedBeforeTheClock) {
    EXPECT_EQ(
        error_of(test_payload({{"iss", R"("https://evil.test")"}, {"exp", numeric_date(-3600)}})),
        AuthError::WrongIssuer);
}

// Claims are read only once the signature holds: a forged token reports its signature, not
// whatever its unauthenticated payload says.
TEST_F(ClaimsTest, AuthenticationJudgesTheSignatureBeforeAnyClaim) {
    const ulw::test::TestKey signer = ulw::test::TestKey::ed25519("k");
    const ulw::test::TestKey forger = ulw::test::TestKey::ed25519("k");
    auto set = infra::auth::detail::parse_key_set(ulw::test::key_set({signer.jwk()}));
    ASSERT_TRUE(set && set->keys.size() == 1);
    const auto& key = set->keys.front();

    const std::string expired = test_payload({{"exp", numeric_date(-3600)}});
    // parse_compact keeps views into the token, so each token outlives its parse.
    const std::string genuine_token = ulw::test::signed_token(signer, "EdDSA", expired);
    const auto genuine = infra::auth::detail::parse_compact(genuine_token);
    ASSERT_TRUE(genuine.has_value());
    EXPECT_EQ(infra::auth::detail::authenticate(*genuine, key, kRules, clock_.wall_now()),
              std::unexpected(AuthError::Expired));

    const std::string forged_token = ulw::test::signed_token(forger, "EdDSA", expired);
    const auto forged = infra::auth::detail::parse_compact(forged_token);
    ASSERT_TRUE(forged.has_value());
    EXPECT_EQ(infra::auth::detail::authenticate(*forged, key, kRules, clock_.wall_now()),
              std::unexpected(AuthError::BadSignature));

    const std::string good_token = ulw::test::signed_token(signer, "EdDSA", test_payload());
    const auto good = infra::auth::detail::parse_compact(good_token);
    ASSERT_TRUE(good.has_value());
    EXPECT_TRUE(infra::auth::detail::authenticate(*good, key, kRules, clock_.wall_now()));
}

} // namespace
