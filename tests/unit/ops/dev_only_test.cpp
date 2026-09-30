#include "ops/dev_only.hpp"

#include <gtest/gtest.h>
#include <map>
#include <string>
#include <utility>

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

} // namespace
