#include "health.hpp"
#include "support/fake_clock.hpp"
#include "support/memory_log.hpp"

#include <gtest/gtest.h>
#include <string>

namespace {

using gateway::Readiness;

class HealthProbeTest : public ::testing::Test {
protected:
    gateway::ProbeChecks checks() {
        return {.database = [this]() -> std::expected<std::optional<core::Seconds>, std::string> {
                    if (!database_error.empty()) {
                        return std::unexpected(database_error);
                    }
                    return oldest;
                },
                .store = [this]() -> std::expected<void, std::string> {
                    if (!store_error.empty()) {
                        return std::unexpected(store_error);
                    }
                    return {};
                },
                .store_paging_errors = [this] { return paging_errors; }};
    }

    ulw::test::FakeClock clock;
    ulw::test::MemoryLog lines;
    ops::Logger log{lines, clock, "gateway", ops::Level::Debug};
    gateway::Health health;
    std::string database_error;
    std::string store_error;
    std::optional<core::Seconds> oldest;
    std::uint64_t paging_errors = 0;
};

TEST_F(HealthProbeTest, ReadyOnlyAfterAProbeFoundBothDependencies) {
    EXPECT_EQ(health.readiness(clock.now()), Readiness::Starting);
    gateway::HealthProbe probe(health, checks(), clock, log);
    probe.probe_once();
    EXPECT_EQ(health.readiness(clock.now()), Readiness::Ready);
    database_error = "connection refused";
    probe.probe_once();
    EXPECT_EQ(health.readiness(clock.now()), Readiness::DatabaseDown);
    database_error.clear();
    store_error = "transient";
    probe.probe_once();
    EXPECT_EQ(health.readiness(clock.now()), Readiness::StoreDown);
}

TEST_F(HealthProbeTest, AnAnswerGoesStaleWhenNoProbeFollowsIt) {
    gateway::HealthProbe probe(health, checks(), clock, log);
    probe.probe_once();
    clock.advance(gateway::kProbeStale);
    EXPECT_EQ(health.readiness(clock.now()), Readiness::Ready);
    clock.advance(core::Millis{1});
    EXPECT_EQ(health.readiness(clock.now()), Readiness::Stale);
}

TEST_F(HealthProbeTest, ADependencyThatStaysDownIsLoggedOnce) {
    gateway::HealthProbe probe(health, checks(), clock, log);
    database_error = "connection refused";
    for (int i = 0; i < 3; ++i) {
        probe.probe_once();
    }
    const auto down = lines.events("dependency down");
    ASSERT_EQ(down.size(), 1U);
    EXPECT_NE(down[0].find(R"("dependency":"database","error":"connection refused")"),
              std::string::npos)
        << down[0];
    database_error.clear();
    probe.probe_once();
    probe.probe_once();
    // The store's first answer, and the database's recovery.
    EXPECT_EQ(lines.events("dependency up").size(), 2U);
}

TEST_F(HealthProbeTest, TheQueueAgeAndStoreCountsAreCarriedOver) {
    gateway::HealthProbe probe(health, checks(), clock, log);
    oldest = core::Seconds{42};
    paging_errors = 3;
    probe.probe_once();
    EXPECT_EQ(health.oldest_queued_seconds(), 42U);
    EXPECT_EQ(health.store_paging_errors(), 3U);
    EXPECT_GT(health.open_fds(), 0U);
    EXPECT_GT(health.resident_bytes(), 0U);
    oldest.reset();
    probe.probe_once();
    EXPECT_EQ(health.oldest_queued_seconds(), 0U);
    // An unreachable database says nothing about the queue; the last age stands.
    oldest = core::Seconds{7};
    probe.probe_once();
    database_error = "gone";
    probe.probe_once();
    EXPECT_EQ(health.oldest_queued_seconds(), 7U);
}

} // namespace
