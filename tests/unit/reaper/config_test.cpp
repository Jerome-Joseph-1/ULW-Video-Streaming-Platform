#include "config.hpp"

#include <gtest/gtest.h>
#include <map>
#include <string>

namespace {

using reaper::StorageBackend;

class ReaperConfigTest : public ::testing::Test {
protected:
    [[nodiscard]] std::expected<reaper::Config, reaper::ConfigError> load() const {
        return reaper::load_config([this](std::string_view name) -> std::optional<std::string> {
            const auto it = env.find(std::string(name));
            return it == env.end() ? std::nullopt : std::optional<std::string>(it->second);
        });
    }

    std::map<std::string, std::string, std::less<>> env{
        {"ULW_DATABASE_URL", "postgresql://ulw@db/ulw"},
        {"ULW_R2_ACCOUNT_ID", "0123456789abcdef0123456789abcdef"},
        {"ULW_BUCKET", "ulw-media"},
    };
};

TEST_F(ReaperConfigTest, OrphansAreSweptAfterTheUploadTtlPlusADay) {
    const auto config = load();
    ASSERT_TRUE(config);
    EXPECT_EQ(config->storage, StorageBackend::R2);
    // Six days a gateway upload may live, and one for a pass that was missed.
    EXPECT_EQ(config->orphan_after, std::chrono::hours(7 * 24));
}

TEST_F(ReaperConfigTest, StayingRootIsAnExplicitChoiceAndTheUserToBecomeOptional) {
    const auto defaults = load();
    ASSERT_TRUE(defaults);
    EXPECT_FALSE(defaults->allow_root);
    EXPECT_TRUE(defaults->run_as_user.empty());
    env["ULW_RUN_AS_USER"] = "ulw";
    env["ULW_ALLOW_ROOT"] = "1";
    const auto set = load();
    ASSERT_TRUE(set);
    EXPECT_EQ(set->run_as_user, "ulw");
    EXPECT_TRUE(set->allow_root);
    env["ULW_ALLOW_ROOT"] = "yes";
    const auto refused = load();
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().variable, "ULW_ALLOW_ROOT");
}

TEST_F(ReaperConfigTest, TheTtlFollowsAGatewayThatChangedIt) {
    env["ULW_UPLOAD_TTL_HOURS"] = "48";
    const auto config = load();
    ASSERT_TRUE(config);
    EXPECT_EQ(config->orphan_after, std::chrono::hours(72));
}

TEST_F(ReaperConfigTest, ARefusedTtlNamesItsVariable) {
    for (const std::string bad : {"0", "-1", "12h", "99999", ""}) {
        env["ULW_UPLOAD_TTL_HOURS"] = bad;
        const auto config = load();
        if (bad.empty()) {
            EXPECT_TRUE(config) << "an empty variable counts as unset";
            continue;
        }
        ASSERT_FALSE(config) << bad;
        EXPECT_EQ(config.error().variable, "ULW_UPLOAD_TTL_HOURS") << bad;
    }
}

TEST_F(ReaperConfigTest, EachRequiredVariableIsNamedWhenMissing) {
    for (const std::string name : {"ULW_DATABASE_URL", "ULW_R2_ACCOUNT_ID", "ULW_BUCKET"}) {
        const std::string saved = env.at(name);
        env.erase(name);
        const auto config = load();
        ASSERT_FALSE(config) << name;
        EXPECT_EQ(config.error().variable, name);
        env[name] = saved;
    }
}

TEST_F(ReaperConfigTest, TheFilesystemBackendNeedsARootAndNoBucket) {
    env = {{"ULW_DATABASE_URL", "postgresql://ulw@db/ulw"},
           {"ULW_STORAGE", "fs"},
           {"ULW_FS_ROOT", "/var/lib/ulw"}};
    const auto config = load();
    ASSERT_TRUE(config);
    EXPECT_EQ(config->storage, StorageBackend::Filesystem);
    EXPECT_EQ(config->storage_location, "/var/lib/ulw");
}

TEST_F(ReaperConfigTest, AnUnknownBackendIsRefused) {
    env["ULW_STORAGE"] = "gcs";
    const auto config = load();
    ASSERT_FALSE(config);
    EXPECT_EQ(config.error().variable, "ULW_STORAGE");
}

} // namespace
