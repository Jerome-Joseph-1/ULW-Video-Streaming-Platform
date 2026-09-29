#include "core/ports/auth.hpp"
#include "infra/auth/base64url.hpp"
#include "infra/auth/local_verifier.hpp"

#include "support/fake_clock.hpp"
#include "test_claims.hpp"
#include "test_keys.hpp"

#include <chrono>
#include <expected>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

using core::ports::AuthError;
using core::ports::VerifyResult;
using infra::auth::Ed25519LocalVerifier;
using infra::auth::KeySetError;
using ulw::test::key_set;
using ulw::test::kTestRules;
using ulw::test::numeric_date;
using ulw::test::signed_token;
using ulw::test::test_payload;
using ulw::test::TestKey;

struct CountingWaiter final : core::ports::IKeyWaiter {
    int calls = 0;
    void on_keys_refreshed() noexcept override { ++calls; }
};

class LocalVerifierTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto created =
            Ed25519LocalVerifier::create(key_set({dev_.jwk(), second_.jwk()}), kTestRules);
        ASSERT_TRUE(created.has_value());
        verifier_.emplace(std::move(*created));
    }

    VerifyResult verify(std::string_view token) {
        const std::optional<VerifyResult> result =
            verifier_->verify(token, clock_.wall_now(), waiter_);
        EXPECT_TRUE(result.has_value()) << "the local verifier deferred";
        return result.value_or(std::unexpected(AuthError::KeysUnavailable));
    }

    TestKey dev_ = TestKey::ed25519("dev");
    TestKey second_ = TestKey::ed25519("dev-2");
    ulw::test::FakeClock clock_;
    CountingWaiter waiter_;
    std::optional<Ed25519LocalVerifier> verifier_;
};

TEST_F(LocalVerifierTest, VerifiesTokensFromEveryConfiguredKey) {
    const VerifyResult first = verify(signed_token(dev_, "EdDSA", test_payload()));
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->subject.view(), "alice");
    EXPECT_EQ(first->email, "alice@example.com");
    const VerifyResult second =
        verify(signed_token(second_, "EdDSA", test_payload({{"sub", R"("bob")"}})));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->subject.view(), "bob");
}

TEST_F(LocalVerifierTest, AnUnknownKidIsAnsweredAtOnceAndNoWaiterIsCalled) {
    const TestKey stranger = TestKey::ed25519("stranger");
    EXPECT_EQ(verify(signed_token(stranger, "EdDSA", test_payload())),
              std::unexpected(AuthError::UnknownKey));
    verifier_->cancel_wait(waiter_);
    EXPECT_EQ(waiter_.calls, 0);
}

TEST_F(LocalVerifierTest, RejectsAlgNone) {
    const std::string token =
        ulw::test::compact(R"({"alg":"none","kid":"dev"})", test_payload(), "");
    EXPECT_EQ(verify(token), std::unexpected(AuthError::UnsupportedAlgorithm));
}

TEST_F(LocalVerifierTest, RejectsHs256KeyedWithThePublicKey) {
    const std::string header = R"({"alg":"HS256","kid":"dev"})";
    const std::string input =
        infra::auth::encode_base64url(header) + '.' + infra::auth::encode_base64url(test_payload());
    const std::string token =
        input + '.' + infra::auth::encode_base64url(ulw::test::hmac_sha256(dev_.jwk(), input));
    EXPECT_EQ(verify(token), std::unexpected(AuthError::UnsupportedAlgorithm));
}

TEST_F(LocalVerifierTest, RejectsASignatureFromAKeyThatMerelyClaimsAConfiguredKid) {
    const TestKey impostor = TestKey::ed25519("dev");
    EXPECT_EQ(verify(signed_token(impostor, "EdDSA", test_payload())),
              std::unexpected(AuthError::BadSignature));
}

TEST_F(LocalVerifierTest, RejectsAPayloadSwappedAfterSigning) {
    const std::string genuine = signed_token(dev_, "EdDSA", test_payload());
    const std::string other = signed_token(dev_, "EdDSA", test_payload({{"sub", R"("root")"}}));
    const std::string spliced = genuine.substr(0, genuine.find('.')) +
                                other.substr(other.find('.'), other.rfind('.') - other.find('.')) +
                                genuine.substr(genuine.rfind('.'));
    ASSERT_NE(spliced, genuine);
    ASSERT_NE(spliced, other);
    EXPECT_EQ(verify(spliced), std::unexpected(AuthError::BadSignature));
}

TEST_F(LocalVerifierTest, AppliesTheClaimRulesAndTheSkewWindow) {
    EXPECT_EQ(verify(signed_token(dev_, "EdDSA", test_payload({{"iss", R"("elsewhere")"}}))),
              std::unexpected(AuthError::WrongIssuer));
    EXPECT_EQ(verify(signed_token(dev_, "EdDSA", test_payload({{"aud", R"("elsewhere")"}}))),
              std::unexpected(AuthError::WrongAudience));

    const std::string expiring = signed_token(dev_, "EdDSA", test_payload());
    clock_.advance(std::chrono::seconds(3600 + 59));
    EXPECT_TRUE(verify(expiring).has_value());
    clock_.advance(std::chrono::seconds(1));
    EXPECT_EQ(verify(expiring), std::unexpected(AuthError::Expired));
}

TEST_F(LocalVerifierTest, ATokenBecomesValidOnlyWithinTheSkewOfItsNotBefore) {
    const std::string early =
        signed_token(dev_, "EdDSA", test_payload({{"nbf", numeric_date(61)}}));
    EXPECT_EQ(verify(early), std::unexpected(AuthError::NotYetValid));
    clock_.advance(std::chrono::seconds(1));
    EXPECT_TRUE(verify(early).has_value());
}

TEST(LocalVerifierSetupTest, RefusesKeySetsItCannotUseWhole) {
    const TestKey ed = TestKey::ed25519("ed");
    const TestKey ec = TestKey::p256("ec");
    EXPECT_EQ(Ed25519LocalVerifier::create("{}", kTestRules).error(), KeySetError::Malformed);
    EXPECT_EQ(Ed25519LocalVerifier::create(key_set({}), kTestRules).error(), KeySetError::Empty);
    EXPECT_EQ(Ed25519LocalVerifier::create(key_set({ed.jwk(), ec.jwk()}), kTestRules).error(),
              KeySetError::UnusableKey);
    EXPECT_EQ(Ed25519LocalVerifier::create(key_set({ed.jwk(), ed.jwk()}), kTestRules).error(),
              KeySetError::UnusableKey);
    EXPECT_EQ(
        Ed25519LocalVerifier::create(key_set({ed.jwk(R"(,"use":"enc")")}), kTestRules).error(),
        KeySetError::UnusableKey);
    EXPECT_TRUE(Ed25519LocalVerifier::create(key_set({ed.jwk()}), kTestRules).has_value());
}

} // namespace
