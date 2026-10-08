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

// 0016 (ADR-0097) builds its table and index before it alters videos, in one ALTER TABLE whose
// checks are NOT VALID: the ACCESS EXCLUSIVE lock on videos, which stops every playback read,
// lasts only until the commit, and nothing scans videos while it is held.
TEST(BundledMigrations, TheVideoAccessMigrationAltersVideosLastAndScansNothing) {
    for (const auto& m : bundled_migrations()) {
        if (m.name != "video_access") {
            continue;
        }
        const std::string_view sql{m.sql};
        const auto at = [&](std::string_view statement, std::size_t from = 0) {
            return sql.find("\n" + std::string(statement), from);
        };
        const auto alter = at("ALTER TABLE");
        ASSERT_NE(at("CREATE TABLE video_grants"), std::string_view::npos);
        ASSERT_NE(at("CREATE INDEX"), std::string_view::npos);
        ASSERT_NE(alter, std::string_view::npos);
        EXPECT_LT(at("CREATE TABLE video_grants"), alter);
        EXPECT_LT(at("CREATE INDEX"), alter);
        EXPECT_EQ(at("ALTER TABLE", alter + 1), std::string_view::npos) << "one ALTER, one lock";
        EXPECT_EQ(at("CREATE", alter), std::string_view::npos)
            << "nothing may be built under the ALTER's lock";
        const std::string_view alter_text = sql.substr(alter);
        std::size_t checks = 0;
        for (auto c = alter_text.find("CHECK"); c != std::string_view::npos;
             c = alter_text.find("CHECK", c + 1)) {
            ++checks;
        }
        std::size_t not_valid = 0;
        for (auto c = alter_text.find("NOT VALID"); c != std::string_view::npos;
             c = alter_text.find("NOT VALID", c + 1)) {
            ++not_valid;
        }
        EXPECT_EQ(checks, 2U);
        EXPECT_EQ(not_valid, checks) << "a validated check would scan videos under the lock";
        return;
    }
    FAIL() << "no video_access migration";
}

// 0017 (ADR-0100) only adds the purge queue: nothing that locks or scans videos, which every
// playback read goes through, not even a foreign key's SHARE ROW EXCLUSIVE lock.
TEST(BundledMigrations, ThePurgeQueueMigrationTouchesNoExistingTable) {
    for (const auto& m : bundled_migrations()) {
        if (m.name != "video_purges") {
            continue;
        }
        const std::string_view sql{m.sql};
        EXPECT_NE(sql.find("\nCREATE TABLE video_purges"), std::string_view::npos);
        EXPECT_EQ(sql.find("\nALTER"), std::string_view::npos);
        EXPECT_EQ(sql.find("REFERENCES"), std::string_view::npos);
        EXPECT_EQ(sql.find(" ON videos"), std::string_view::npos);
        return;
    }
    FAIL() << "no video_purges migration";
}

} // namespace
