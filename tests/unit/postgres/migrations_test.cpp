#include "infra/postgres/migrator.hpp"

#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <string_view>

namespace {

using infra::postgres::bundled_migrations;

TEST(BundledMigrations, VersionsRunFromOneWithoutGaps) {
    const auto migrations = bundled_migrations();
    ASSERT_FALSE(migrations.empty());
    int expected = 1;
    for (const auto& m : migrations) {
        EXPECT_EQ(m.version, expected++);
        EXPECT_FALSE(m.name.empty());
    }
}

TEST(BundledMigrations, CarryEachFileByteForByte) {
    for (const auto& m : bundled_migrations()) {
        const std::string path =
            std::format("{}/{:04}_{}.sql", ULW_MIGRATIONS_DIR, m.version, m.name);
        const std::ifstream in(path, std::ios::binary);
        ASSERT_TRUE(in) << path;
        std::ostringstream file;
        file << in.rdbuf();
        EXPECT_EQ(std::string{m.sql}, file.str()) << path;
    }
}

// The migrator holds every lock a migration takes until it commits (ADR-0031). 0015 builds its
// index on chat_members before any ALTER TABLE, so that the build runs under CREATE INDEX's SHARE
// lock (reads proceed, writes wait) and the ALTERs' ACCESS EXCLUSIVE locks last only until the
// commit right after them, not through the build.
TEST(BundledMigrations, TheMembershipMigrationBuildsItsIndexBeforeAnyAlter) {
    for (const auto& m : bundled_migrations()) {
        if (m.name != "chat_membership") {
            continue;
        }
        const std::string_view sql{m.sql};
        // Statements start a line; comments mention them too.
        const auto at = [&](std::string_view statement) {
            return sql.find("\n" + std::string(statement));
        };
        ASSERT_NE(at("CREATE INDEX"), std::string_view::npos);
        ASSERT_NE(at("ALTER TABLE"), std::string_view::npos);
        EXPECT_LT(at("CREATE INDEX"), at("ALTER TABLE"));
        EXPECT_LT(at("ALTER TABLE"), at("CREATE TRIGGER"));
        EXPECT_EQ(sql.find("\nCREATE INDEX", at("CREATE INDEX") + 1), std::string_view::npos)
            << "a second index would be built under the ALTERs' locks";
        return;
    }
    FAIL() << "no chat_membership migration";
}

} // namespace
