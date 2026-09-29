#include "infra/postgres/migrator.hpp"

#include "postgres_harness.hpp"

#include <algorithm>
#include <array>
#include <expected>
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>

namespace {

using infra::postgres::bundled_migrations;
using infra::postgres::Migration;
using infra::postgres::Migrator;
using ulw::test::scalar;
using ulw::test::Schema;
using ulw::test::ScratchDatabase;

class MigrateTest : public ::testing::Test {
protected:
    void SetUp() override { ScratchDatabase::open(db, Schema::Empty); }

    Migrator connect() {
        auto m = Migrator::connect(db->conninfo());
        EXPECT_TRUE(m) << m.error().message;
        return std::move(*m);
    }

    std::string count_applied() {
        auto conn = db->session();
        return scalar(conn, "SELECT count(*) FROM schema_migrations");
    }

    static std::string count_bundled() { return std::to_string(bundled_migrations().size()); }

    // The first version after every bundled one.
    static int next_version() { return bundled_migrations().back().version + 1; }

    std::unique_ptr<ScratchDatabase> db;
};

TEST_F(MigrateTest, AppliesTheSchemaAndRecordsEachVersion) {
    Migrator migrator = connect();
    const auto applied = migrator.apply(bundled_migrations());
    ASSERT_TRUE(applied) << applied.error().message;
    std::vector<int> every;
    for (const Migration& m : bundled_migrations()) {
        every.push_back(m.version);
    }
    EXPECT_EQ(*applied, every);
    auto conn = db->session();
    EXPECT_EQ(scalar(conn, "SELECT count(*) FROM information_schema.tables "
                           "WHERE table_name IN ('videos', 'uploads', 'jobs', 'renditions')"),
              "4");
    EXPECT_EQ(scalar(conn, "SELECT name FROM schema_migrations WHERE version = 1"), "initial");
}

TEST_F(MigrateTest, SecondRunFindsNothingToDo) {
    Migrator migrator = connect();
    ASSERT_TRUE(migrator.apply(bundled_migrations()));
    const auto again = connect().apply(bundled_migrations());
    ASSERT_TRUE(again) << again.error().message;
    EXPECT_TRUE(again->empty());
    EXPECT_EQ(count_applied(), count_bundled());
}

TEST_F(MigrateTest, StatusListsPendingThenApplied) {
    Migrator migrator = connect();
    auto before = migrator.status(bundled_migrations());
    ASSERT_TRUE(before) << before.error().message;
    ASSERT_EQ(before->size(), bundled_migrations().size());
    EXPECT_FALSE(before->front().applied_at);
    ASSERT_TRUE(migrator.apply(bundled_migrations()));
    auto after = migrator.status(bundled_migrations());
    ASSERT_TRUE(after);
    ASSERT_TRUE(after->front().applied_at);
    EXPECT_FALSE(after->front().unknown);
}

TEST_F(MigrateTest, RefusesToRunWhileAnotherMigratorHoldsTheLock) {
    auto holder = db->session();
    ASSERT_EQ(scalar(holder, "SELECT pg_try_advisory_lock(7695479, 1)"), "t");
    const auto refused = connect().apply(bundled_migrations());
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().message, "another migrator is running");
    ASSERT_EQ(scalar(holder, "SELECT pg_advisory_unlock(7695479, 1)"), "t");
    EXPECT_TRUE(connect().apply(bundled_migrations()));
}

TEST_F(MigrateTest, ConcurrentMigratorsApplyEachVersionOnce) {
    constexpr int kMigrators = 4;
    std::vector<Migrator> migrators;
    migrators.reserve(kMigrators);
    for (int i = 0; i < kMigrators; ++i) {
        migrators.push_back(connect());
    }
    std::vector<std::expected<std::vector<int>, infra::postgres::MigrationError>> results(
        kMigrators);
    {
        std::vector<std::jthread> threads;
        threads.reserve(migrators.size());
        for (std::size_t i = 0; i < migrators.size(); ++i) {
            threads.emplace_back([&, i] { results[i] = migrators[i].apply(bundled_migrations()); });
        }
    }
    int applied_initial = 0;
    for (const auto& r : results) {
        if (r) {
            applied_initial += static_cast<int>(std::ranges::count(*r, 1));
        } else {
            EXPECT_EQ(r.error().message, "another migrator is running");
        }
    }
    // Whoever lost the race either saw the lock taken or found the work done.
    EXPECT_EQ(applied_initial, 1);
    EXPECT_EQ(count_applied(), count_bundled());
}

TEST_F(MigrateTest, FailingMigrationLeavesNoTrace) {
    const std::array known{
        bundled_migrations().front(),
        Migration{.version = 2,
                  .name = "broken",
                  .sql = "CREATE TABLE half_done (x integer); SELECT 1 / 0;"},
    };
    const auto result = connect().apply(known);
    ASSERT_FALSE(result);
    EXPECT_TRUE(result.error().message.starts_with("0002_broken: ")) << result.error().message;
    auto conn = db->session();
    EXPECT_EQ(scalar(conn, "SELECT to_regclass('half_done') IS NULL"), "t");
    EXPECT_EQ(count_applied(), "1");
}

TEST_F(MigrateTest, RefusesAPendingVersionOlderThanOneApplied) {
    const Migration first = bundled_migrations().front();
    const Migration second{.version = 2, .name = "second", .sql = "SELECT 1"};
    const Migration third{.version = 3, .name = "third", .sql = "SELECT 1"};
    ASSERT_TRUE(connect().apply(std::array{first, third}));
    const auto result = connect().apply(std::array{first, second, third});
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message.find("0002_second is older than applied version 0003"),
              std::string::npos)
        << result.error().message;
}

TEST_F(MigrateTest, DdlWaitingOnALockGivesUpInsteadOfStallingTraffic) {
    ASSERT_TRUE(connect().apply(bundled_migrations()));
    auto reader = db->session();
    ASSERT_TRUE(reader.exec("BEGIN"));
    ASSERT_TRUE(reader.exec("SELECT count(*) FROM videos"));
    std::vector<Migration> known(bundled_migrations().begin(), bundled_migrations().end());
    known.push_back(Migration{.version = next_version(),
                              .name = "add_column",
                              .sql = "ALTER TABLE videos ADD c integer"});
    const auto result = connect().apply(known);
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message.find("lock timeout"), std::string::npos)
        << result.error().message;
    ASSERT_TRUE(reader.exec("ROLLBACK"));
    EXPECT_TRUE(connect().apply(known));
}

TEST_F(MigrateTest, CommandLineReportsOutcomeInItsExitCode) {
    const std::string url = "ULW_DATABASE_URL=" + db->conninfo();
    // Some runs start tests as root; this suite is not about that.
    const std::string root = "ULW_ALLOW_ROOT=1";
    EXPECT_EQ(ulw::test::run_process({ULW_MIGRATE_BIN, "--frobnicate"}, {url}).exit_code, 2);
    EXPECT_EQ(ulw::test::run_process({"env", "-u", "ULW_DATABASE_URL", ULW_MIGRATE_BIN}).exit_code,
              2);
    const auto applied = ulw::test::run_process({ULW_MIGRATE_BIN}, {url, root});
    EXPECT_EQ(applied.exit_code, 0) << applied.output;
    EXPECT_NE(applied.output.find("applied 0001_initial"), std::string::npos) << applied.output;
    const auto status = ulw::test::run_process({ULW_MIGRATE_BIN, "--status"}, {url, root});
    EXPECT_EQ(status.exit_code, 0) << status.output;
    EXPECT_TRUE(status.output.starts_with("0001 initial")) << status.output;
    EXPECT_NE(status.output.find(" applied "), std::string::npos) << status.output;
    const auto unreachable = ulw::test::run_process(
        {ULW_MIGRATE_BIN}, {"ULW_DATABASE_URL=postgresql://nobody@127.0.0.1:1/none", root});
    EXPECT_EQ(unreachable.exit_code, 1) << unreachable.output;
}

TEST_F(MigrateTest, APasswordNeverReachesTheOutput) {
    for (const std::string url : {"postgresql://ulw:Sup3r%Secret@127.0.0.1:1/ulw",
                                  "postgresql://ulw:Sup3rSecret@127.0.0.1:1/ulw"}) {
        const auto r = ulw::test::run_process({ULW_MIGRATE_BIN},
                                              {"ULW_DATABASE_URL=" + url, "ULW_ALLOW_ROOT=1"});
        EXPECT_EQ(r.exit_code, 1) << r.output;
        EXPECT_EQ(r.output.find("Sup3r"), std::string::npos) << r.output;
    }
}

} // namespace
