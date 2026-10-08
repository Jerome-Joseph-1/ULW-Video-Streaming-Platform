#include "core/ports/auth.hpp"
#include "infra/auth/local_verifier.hpp"

#include "devtoken/dev_key.hpp"
#include "devtoken/key_file.hpp"
#include "support/fake_clock.hpp"
#include "test_claims.hpp"

#include <sys/stat.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <gtest/gtest.h>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
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

// A backend's token for chat's service API, as a client-credentials grant gives one.
TEST(DevTokenTest, AScopeIsMintedAsTheSpaceSeparatedClaimTheServiceRuleReads) {
    const ulw::test::FakeClock clock;
    const auto key = DevKey::generate();
    ASSERT_TRUE(key.has_value());
    MintRequest request = request_for("backend");
    request.scope = "openid ulw:admin";
    const auto token = key->mint(request, clock.wall_now());
    ASSERT_TRUE(token.has_value());
    infra::auth::ClaimRules rules = ulw::test::kTestRules;
    rules.service_claim = "scope";
    rules.service_value = "ulw:admin";
    auto verifier = infra::auth::Ed25519LocalVerifier::create(key->public_jwks(), rules);
    ASSERT_TRUE(verifier);
    NoWaiter waiter;
    const auto claims = verifier->verify(*token, clock.wall_now(), waiter);
    ASSERT_TRUE(claims && *claims);
    EXPECT_TRUE((*claims)->is_service);
    const auto plain = key->mint(request_for("backend"), clock.wall_now());
    ASSERT_TRUE(plain.has_value());
    const auto user = verifier->verify(*plain, clock.wall_now(), waiter);
    ASSERT_TRUE(user && *user);
    EXPECT_FALSE((*user)->is_service);
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

TEST(DevTokenTest, TheTtlIsWholeSecondsUpToAWeek) {
    EXPECT_EQ(devtoken::parse_ttl("1"), std::chrono::seconds(1));
    EXPECT_EQ(devtoken::parse_ttl("604800"), devtoken::kMaxTtl);
    for (const std::string_view bad : {"", "0", "-5", "604801", "1.5", "60s", " 60"}) {
        EXPECT_FALSE(devtoken::parse_ttl(bad).has_value()) << bad;
    }
}

// Each test gets its own directory, removed afterwards.
class KeyFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string tmpl =
            (std::filesystem::temp_directory_path() / "ulw-devtoken-XXXXXX").string();
        ASSERT_NE(::mkdtemp(tmpl.data()), nullptr);
        dir = tmpl;
    }
    void TearDown() override { std::filesystem::remove_all(dir); }

    [[nodiscard]] std::string path(std::string_view name) const { return (dir / name).string(); }

    std::filesystem::path dir;
};

TEST_F(KeyFileTest, ANewKeyFileIsOwnerOnlyWhateverTheUmask) {
    const ::mode_t saved = ::umask(0);
    const auto written = devtoken::write_new_key_file(path("k"), "secret");
    ::umask(saved);
    ASSERT_TRUE(written);
    struct stat st {};
    ASSERT_EQ(::stat(path("k").c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777U, 0600U);
    EXPECT_EQ(devtoken::read_key_file(path("k")), "secret");
}

TEST_F(KeyFileTest, AnExistingKeyFileIsNeverReplaced) {
    ASSERT_TRUE(devtoken::write_new_key_file(path("k"), "first"));
    EXPECT_EQ(devtoken::write_new_key_file(path("k"), "second").error(), EEXIST);
    EXPECT_EQ(devtoken::read_key_file(path("k")), "first");
}

TEST_F(KeyFileTest, AKeyOthersCanReadIsRefused) {
    ASSERT_TRUE(devtoken::write_new_key_file(path("k"), "secret"));
    for (const ::mode_t mode : {0640U, 0604U, 0660U}) {
        ASSERT_EQ(::chmod(path("k").c_str(), mode), 0);
        EXPECT_EQ(devtoken::read_key_file(path("k")).error(), EPERM) << std::oct << mode;
    }
}

TEST_F(KeyFileTest, ASymlinkIsNotFollowed) {
    ASSERT_TRUE(devtoken::write_new_key_file(path("real"), "secret"));
    ASSERT_EQ(::symlink(path("real").c_str(), path("link").c_str()), 0);
    EXPECT_EQ(devtoken::read_key_file(path("link")).error(), ELOOP);
    EXPECT_EQ(devtoken::write_new_key_file(path("link"), "other").error(), EEXIST);
}

TEST_F(KeyFileTest, AnOversizedFileIsNotAKey) {
    ASSERT_TRUE(devtoken::write_new_key_file(path("big"),
                                             std::string(devtoken::kMaxKeyFileBytes + 1, 'k')));
    EXPECT_EQ(devtoken::read_key_file(path("big")).error(), EFBIG);
    ASSERT_TRUE(
        devtoken::write_new_key_file(path("max"), std::string(devtoken::kMaxKeyFileBytes, 'k')));
    EXPECT_TRUE(devtoken::read_key_file(path("max")).has_value());
}

} // namespace
