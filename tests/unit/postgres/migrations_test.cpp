#include "infra/postgres/migrator.hpp"

#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>

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

} // namespace
