#include "ops/dev_only.hpp"

#include <expected>
#include <gtest/gtest.h>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

class DevOnlyTest : public ::testing::Test {
protected:
    [[nodiscard]] std::expected<void, ops::DevOnlyRefusal> allow() const {
        return ops::allow_dev_only(
            "ULW_DEV_JWKS_FILE", [this](std::string_view name) -> std::optional<std::string> {
                const auto it = env.find(name);
                return it == env.end() ? std::nullopt : std::optional(it->second);
            });
    }

    [[nodiscard]] std::string refused_variable() const {
        const auto r = allow();
        EXPECT_FALSE(r.has_value());
        return r ? std::string() : r.error().variable;
    }

    std::map<std::string, std::string, std::less<>> env;
};

TEST_F(DevOnlyTest, UnsetThereIsNothingToAllow) {
    EXPECT_TRUE(allow());
    env["KUBERNETES_SERVICE_HOST"] = "10.43.0.1";
    EXPECT_TRUE(allow());
    env["ULW_DEV_JWKS_FILE"] = "";
    EXPECT_TRUE(allow());
}

TEST_F(DevOnlyTest, ASettingForDevelopmentNeedsDevelopmentModeSaidOutright) {
    env["ULW_DEV_JWKS_FILE"] = "/etc/ulw/dev-jwks.json";
    EXPECT_EQ(refused_variable(), "ULW_DEV_JWKS_FILE");
    env["ULW_DEV_MODE"] = "0";
    EXPECT_EQ(refused_variable(), "ULW_DEV_JWKS_FILE");
    env["ULW_DEV_MODE"] = "1";
    EXPECT_TRUE(allow());
}

TEST_F(DevOnlyTest, InsideAKubernetesPodItIsRefusedEvenInDevelopmentMode) {
    env["ULW_DEV_JWKS_FILE"] = "/etc/ulw/dev-jwks.json";
    env["ULW_DEV_MODE"] = "1";
    env["KUBERNETES_SERVICE_HOST"] = "10.43.0.1";
    const auto r = allow();
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().variable, "ULW_DEV_JWKS_FILE");
    EXPECT_NE(r.error().reason.find("Kubernetes"), std::string::npos);
}

TEST_F(DevOnlyTest, DevelopmentModeIsZeroOrOne) {
    for (const char* bad : {"true", "yes", "01", "development"}) {
        env["ULW_DEV_MODE"] = bad;
        EXPECT_EQ(refused_variable(), "ULW_DEV_MODE") << bad;
    }
}

// Both servers take their keys the same way: one source, https or a key set development allows.
TEST_F(DevOnlyTest, KeysComeFromExactlyOneSource) {
    const auto source = [this] {
        return ops::key_source([this](std::string_view name) -> std::optional<std::string> {
            const auto it = env.find(name);
            return it == env.end() ? std::nullopt : std::optional(it->second);
        });
    };
    // The variable a refusal names, and why.
    const auto refusal = [&source]() -> std::pair<std::string, std::string> {
        const auto result = source();
        EXPECT_FALSE(result);
        if (result) {
            return {};
        }
        return {result.error().variable, result.error().reason};
    };
    using Refusal = std::pair<std::string, std::string>;
    EXPECT_EQ(refusal(), (Refusal{"JWKS_URL", "not set"}));
    env["JWKS_URL"] = "http://auth.example.test/jwks.json";
    EXPECT_EQ(refusal(), (Refusal{"JWKS_URL", "must be an https URL"}));
    env["JWKS_URL"] = "https://auth.example.test/jwks.json";
    const auto fetched = source();
    ASSERT_TRUE(fetched);
    EXPECT_EQ(fetched->url, "https://auth.example.test/jwks.json");
    EXPECT_TRUE(fetched->file.empty());
    env["ULW_DEV_JWKS_FILE"] = "/etc/ulw/dev-jwks.json";
    EXPECT_EQ(refusal(), (Refusal{"JWKS_URL", "set together with ULW_DEV_JWKS_FILE; choose one"}));
    env["JWKS_URL"] = "";
    EXPECT_EQ(refusal(),
              (Refusal{"ULW_DEV_JWKS_FILE",
                       "development only; set ULW_DEV_MODE=1 where that is what this is"}));
    env["ULW_DEV_MODE"] = "1";
    const auto local = source();
    ASSERT_TRUE(local);
    EXPECT_EQ(local->file, "/etc/ulw/dev-jwks.json");
    EXPECT_TRUE(local->url.empty());
}

class TokenRulesTest : public ::testing::Test {
protected:
    [[nodiscard]] std::expected<ops::TokenRules, ops::DevOnlyRefusal>
    rules(const ops::KeySource& keys) const {
        return ops::token_rules(
            [this](std::string_view name) -> std::optional<std::string> {
                const auto it = env.find(name);
                return it == env.end() ? std::nullopt : std::optional(it->second);
            },
            keys);
    }

    std::map<std::string, std::string, std::less<>> env;
    const ops::KeySource jwks{.url = "https://auth.example.test/jwks.json", .file = ""};
    const ops::KeySource local{.url = "", .file = "/etc/ulw/dev-jwks.json"};
};

// No identity provider's audience is a safe guess, so a JWKS takes it said outright.
TEST_F(TokenRulesTest, AJwksNeedsTheAudienceAndALocalKeySetDefaultsIt) {
    auto r = rules(jwks);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().variable, "JWT_AUDIENCE");
    env["JWT_AUDIENCE"] = "";
    EXPECT_FALSE(rules(jwks));
    r = rules(local);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->audience, ops::kDevAudience);
    env["JWT_AUDIENCE"] = "ulw-test-audience";
    r = rules(jwks);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->audience, "ulw-test-audience");
    r = rules(local);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->audience, "ulw-test-audience");
}

TEST_F(TokenRulesTest, TheSubjectClaimIsSubUnlessNamedAndAClaimNameWhenNamed) {
    env["JWT_AUDIENCE"] = "ulw-test-audience";
    auto r = rules(jwks);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->subject_claim, "sub");
    env["ULW_JWT_SUBJECT_CLAIM"] = "";
    r = rules(jwks);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->subject_claim, "sub");
    for (const std::string& good : std::vector<std::string>{
             "user_id", "https://example.com/claims/uid", "a.b-c:d", std::string(64, 'x')}) {
        env["ULW_JWT_SUBJECT_CLAIM"] = good;
        r = rules(jwks);
        ASSERT_TRUE(r) << good;
        EXPECT_EQ(r->subject_claim, good);
    }
    for (const std::string& bad :
         std::vector<std::string>{"user id", "sub\"", "sub\\", "{sub}", std::string(65, 'x')}) {
        env["ULW_JWT_SUBJECT_CLAIM"] = bad;
        r = rules(jwks);
        ASSERT_FALSE(r) << bad;
        EXPECT_EQ(r.error().variable, "ULW_JWT_SUBJECT_CLAIM");
    }
}

} // namespace
