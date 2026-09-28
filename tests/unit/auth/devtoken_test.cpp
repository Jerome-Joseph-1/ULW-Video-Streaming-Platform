#include "core/ports/auth.hpp"
#include "infra/auth/local_verifier.hpp"

#include "devtoken/dev_key.hpp"
#include "support/fake_clock.hpp"
#include "test_claims.hpp"

#include <chrono>
#include <expected>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

using core::ports::AuthError;
using devtoken::DevKey;
using devtoken::DevKeyError;
using devtoken::MintRequest;

// RFC 8037 appendix A.1, whose thumbprint appendix A.3 gives.
constexpr std::string_view kRfc8037Key = R"({"kty":"OKP","crv":"Ed25519",)"
                                         R"("d":"nWGxne_9WmC6hEr0kuwsxERJxWl7MmkZcDusAxyuf2A",)"
                                         R"("x":"11qYAYKxCrfVS_7TyWQHOg7hcvPapiMlrwIaaPcHURo"})";
constexpr std::string_view kRfc8037Thumbprint = "kPrK_qmxVWaYVA9wwBF6Iuo3vVzz7TxHCTwXBygrS4k";

struct NoWaiter final : core::ports::IKeyWaiter {
    void on_keys_refreshed() noexcept override {}
};

MintRequest request_for(std::string subject, std::string email = {}) {
    return {.issuer = ulw::test::kTestRules.issuer,
            .audience = ulw::test::kTestRules.audience,
            .subject = std::move(subject),
            .email = std::move(email),
            .ttl = std::chrono::minutes(30)};
}

core::ports::VerifyResult verify_with(const DevKey& key, const std::string& token,
                                      core::WallTime now) {
    auto verifier =
        infra::auth::Ed25519LocalVerifier::create(key.public_jwks(), ulw::test::kTestRules);
    if (!verifier) {
        ADD_FAILURE() << "public key set refused: " << infra::auth::to_string(verifier.error());
        return std::unexpected(AuthError::KeysUnavailable);
    }
    NoWaiter waiter;
    return verifier->verify(token, now, waiter)
        .value_or(std::unexpected(AuthError::KeysUnavailable));
}

TEST(DevTokenTest, AMintedTokenVerifiesWithTheLocalVerifier) {
    const ulw::test::FakeClock clock;
    const auto key = DevKey::generate();
    ASSERT_TRUE(key.has_value());
    const auto token = key->mint(request_for("dev-user", "dev@example.com"), clock.wall_now());
    ASSERT_TRUE(token.has_value());

    const core::ports::VerifyResult claims = verify_with(*key, *token, clock.wall_now());
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->subject.view(), "dev-user");
    EXPECT_EQ(claims->email, "dev@example.com");
    EXPECT_EQ(claims->expires_at, clock.wall_now() + std::chrono::minutes(30));
}

TEST(DevTokenTest, NoEmailClaimWhenNoneIsGiven) {
    const ulw::test::FakeClock clock;
    const auto key = DevKey::generate();
    ASSERT_TRUE(key.has_value());
    const auto token = key->mint(request_for("dev-user"), clock.wall_now());
    ASSERT_TRUE(token.has_value());
    const core::ports::VerifyResult claims = verify_with(*key, *token, clock.wall_now());
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->email, "");
}

TEST(DevTokenTest, AMintedTokenLapsesAfterItsTtl) {
    ulw::test::FakeClock clock;
    const auto key = DevKey::generate();
    ASSERT_TRUE(key.has_value());
    const auto token = key->mint(request_for("dev-user"), clock.wall_now());
    ASSERT_TRUE(token.has_value());
    clock.advance(std::chrono::minutes(31));
    EXPECT_EQ(verify_with(*key, *token, clock.wall_now()), std::unexpected(AuthError::Expired));
}

TEST(DevTokenTest, TheKidIsTheRfc7638Thumbprint) {
    const auto key = DevKey::from_private_jwk(kRfc8037Key);
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(key->kid(), kRfc8037Thumbprint);
}

TEST(DevTokenTest, APrivateJwkLoadsBackAsTheSameKey) {
    const ulw::test::FakeClock clock;
    const auto original = DevKey::generate();
    ASSERT_TRUE(original.has_value());
    const auto jwk = original->private_jwk();
    ASSERT_TRUE(jwk.has_value());
    const auto reloaded = DevKey::from_private_jwk(*jwk);
    ASSERT_TRUE(reloaded.has_value());
    EXPECT_EQ(reloaded->kid(), original->kid());
    const auto token = reloaded->mint(request_for("dev-user"), clock.wall_now());
    ASSERT_TRUE(token.has_value());
    EXPECT_TRUE(verify_with(*original, *token, clock.wall_now()).has_value());
}

TEST(DevTokenTest, RefusesKeyFilesThatAreNotItsOwn) {
    std::string mismatched{kRfc8037Key};
    mismatched.replace(mismatched.find("11qY"), 4, "22qY");
    EXPECT_EQ(DevKey::from_private_jwk(mismatched).error(), DevKeyError::Mismatch);

    EXPECT_EQ(DevKey::from_private_jwk("not json").error(), DevKeyError::Malformed);
    EXPECT_EQ(DevKey::from_private_jwk(R"({"kty":"RSA","n":"AQAB","e":"AQAB"})").error(),
              DevKeyError::Malformed);
    EXPECT_EQ(
        DevKey::from_private_jwk(
            R"({"kty":"OKP","crv":"Ed25519","x":"11qYAYKxCrfVS_7TyWQHOg7hcvPapiMlrwIaaPcHURo"})")
            .error(),
        DevKeyError::Malformed);
    EXPECT_EQ(
        DevKey::from_private_jwk(R"({"kty":"OKP","crv":"Ed25519","d":"AAAA","x":"AAAA"})").error(),
        DevKeyError::Malformed);
}

} // namespace
