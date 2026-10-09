#include "e2ee_directory_contract.hpp"
#include "pg_directory_harness.hpp"
#include "postgres_harness.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"
#include "support/reactor_harness.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

using core::ports::E2eeError;
using core::ports::FetchedKeyPackage;
using core::ports::KeyPackageBytes;
using infra::postgres::Params;
using infra::postgres::SyncConnection;
using ulw::test::Answer;
using ulw::test::DirectoryContract;
using ulw::test::DirectoryFactory;
using ulw::test::DirectoryHarness;
using ulw::test::package_of;
using ulw::test::PgDirectoryHarness;
using ulw::test::scalar;

// Each fetcher of a race needs a session of its own, so the races run as many as the pool
// allows. 32 contenders on one row is far more than any real invitation burst.
constexpr std::size_t kContenders = 32;

INSTANTIATE_TEST_SUITE_P(Postgres, DirectoryContract,
                         ::testing::Values(DirectoryFactory{
                             .name = "postgres",
                             .make = []() -> std::unique_ptr<DirectoryHarness> {
                                 return PgDirectoryHarness::open(4);
                             }}),
                         [](const auto& p) { return p.param.name; });

// Races on one device's row. Every contender queues behind a lock the test holds on the device
// row, and the test lets go only once all of them are waiting on it in their own sessions, so
// they all hit the package rows in the same instant rather than one after another.
class E2eeRaceTest : public ::testing::Test {
protected:
    void SetUp() override {
        harness_ = PgDirectoryHarness::open(kContenders);
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        ASSERT_NE(harness_, nullptr);
        Answer<void> enrolled;
        harness_->registry().register_device(alice_, device_, enrolled.callback());
        ASSERT_TRUE(ulw::test::await(harness_->reactor(), enrolled));
    }

    void stock(std::size_t count) {
        std::vector<KeyPackageBytes> batch;
        batch.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            batch.push_back(package_of(300, static_cast<std::uint8_t>(i)));
        }
        Answer<std::size_t> published;
        harness_->delivery().publish_key_packages(alice_, device_, batch, published.callback());
        ASSERT_EQ(ulw::test::await(harness_->reactor(), published), count);
    }

    // Holds the device row until release(): FOR UPDATE conflicts with every lock the directory
    // takes on it.
    void hold_device() {
        gate_.emplace(harness_->db().session());
        ASSERT_TRUE(gate_->exec("BEGIN"));
        ASSERT_TRUE(gate_->exec("SELECT 1 FROM devices WHERE id = $1 FOR UPDATE",
                                Params{}.add_uuid(device_.uuid())));
    }

    // Pumps until `n` of the directory's sessions are blocked on a lock, then lets them go.
    void release_when_waiting(std::size_t n) {
        auto observer = harness_->db().session();
        const std::string want = std::to_string(n);
        const bool queued = ulw::test::pump_until(
            harness_->reactor(),
            [&] {
                return scalar(observer, R"sql(
SELECT count(*) FROM pg_stat_activity
 WHERE datname = current_database() AND application_name = 'ulw-e2ee'
   AND wait_event_type = 'Lock')sql") == want;
            },
            std::chrono::seconds(20));
        ASSERT_TRUE(queued) << "the contenders never all queued behind the gate";
        ASSERT_TRUE(gate_->exec("COMMIT"));
    }

    template <class T> void await_all(std::vector<Answer<T>>& answers) {
        const bool all = ulw::test::pump_until(
            harness_->reactor(),
            [&] { return std::ranges::all_of(answers, [](const auto& a) { return a.ready(); }); },
            std::chrono::seconds(30));
        ASSERT_TRUE(all) << "a contender never answered";
    }

    std::string packages_left() {
        auto conn = harness_->db().session();
        return scalar(conn, "SELECT count(*) FROM key_packages WHERE device_id = $1",
                      Params{}.add_uuid(device_.uuid()));
    }

    ulw::test::FakeClock clock_;
    ulw::test::FakeRandom random_;
    const core::UserId alice_ = *core::UserId::parse("auth0|alice");
    core::DeviceId device_ = core::DeviceId::generate(clock_, random_);
    std::unique_ptr<PgDirectoryHarness> harness_;
    std::optional<SyncConnection> gate_;
};

TEST_F(E2eeRaceTest, ConcurrentFetchersOfTheLastPackageHaveExactlyOneWinner) {
    stock(1);
    hold_device();
    std::vector<Answer<FetchedKeyPackage>> answers(kContenders);
    for (auto& a : answers) {
        harness_->delivery().fetch_key_package(alice_, device_, a.callback());
    }
    release_when_waiting(kContenders);
    await_all(answers);

    std::size_t won = 0;
    for (const auto& a : answers) {
        EXPECT_EQ(a.calls(), 1);
        if (a.get()) {
            ++won;
            EXPECT_EQ(a.get()->package, package_of(300, 0));
            EXPECT_TRUE(a.get()->replenish);
        } else {
            EXPECT_EQ(a.get().error(), E2eeError::Exhausted);
        }
    }
    EXPECT_EQ(won, 1U);
    EXPECT_EQ(packages_left(), "0");
}

// ADR-0101: with a last-resort package behind the last single-use one, nobody comes away empty,
// and still only one contender gets the single-use package.
TEST_F(E2eeRaceTest, ConcurrentFetchersOfTheLastPackageFallBackToTheLastResort) {
    stock(1);
    Answer<void> kept;
    harness_->delivery().publish_last_resort(alice_, device_, package_of(400, 99), kept.callback());
    ASSERT_TRUE(ulw::test::await(harness_->reactor(), kept));
    hold_device();
    std::vector<Answer<FetchedKeyPackage>> answers(kContenders);
    for (auto& a : answers) {
        harness_->delivery().fetch_key_package(alice_, device_, a.callback());
    }
    release_when_waiting(kContenders);
    await_all(answers);

    std::size_t single = 0;
    std::size_t last_resort = 0;
    for (const auto& a : answers) {
        ASSERT_TRUE(a.get()) << to_string(a.get().error());
        EXPECT_TRUE(a.get()->replenish);
        if (a.get()->last_resort) {
            ++last_resort;
            EXPECT_EQ(a.get()->package, package_of(400, 99));
        } else {
            ++single;
            EXPECT_EQ(a.get()->package, package_of(300, 0));
        }
    }
    EXPECT_EQ(single, 1U);
    EXPECT_EQ(last_resort, kContenders - 1);
    EXPECT_EQ(packages_left(), "0");
    auto conn = harness_->db().session();
    EXPECT_EQ(scalar(conn,
                     "SELECT served_at IS NOT NULL FROM last_resort_key_packages "
                     "WHERE device_id = $1",
                     Params{}.add_uuid(device_.uuid())),
              "t");
}

// A fetch that deletes the oldest row without locking it first loses this one: all contenders
// pick the same row, and every loser returns empty-handed although packages remain.
TEST_F(E2eeRaceTest, ConcurrentFetchersEachTakeADifferentPackageUntilNoneRemain) {
    constexpr std::size_t kStock = 20;
    stock(kStock);
    hold_device();
    std::vector<Answer<FetchedKeyPackage>> answers(kContenders);
    for (auto& a : answers) {
        harness_->delivery().fetch_key_package(alice_, device_, a.callback());
    }
    release_when_waiting(kContenders);
    await_all(answers);

    std::set<KeyPackageBytes> handed_out;
    std::size_t exhausted = 0;
    for (const auto& a : answers) {
        if (a.get()) {
            EXPECT_TRUE(handed_out.insert(a.get()->package).second) << "handed out twice";
        } else {
            EXPECT_EQ(a.get().error(), E2eeError::Exhausted);
            ++exhausted;
        }
    }
    EXPECT_EQ(handed_out.size(), kStock);
    EXPECT_EQ(exhausted, kContenders - kStock);
    EXPECT_EQ(packages_left(), "0");
}

TEST_F(E2eeRaceTest, ConcurrentPublishesNeverTakeTheSupplyPastTheCap) {
    // Eight batches of a fifth of the cap: exactly five fit, whatever order they land in.
    constexpr std::size_t kPublishers = 8;
    constexpr std::size_t kBatch = core::ports::kMaxKeyPackagesPerDevice / 5;
    hold_device();
    std::vector<Answer<std::size_t>> answers(kPublishers);
    for (std::size_t i = 0; i < kPublishers; ++i) {
        std::vector<KeyPackageBytes> batch;
        batch.reserve(kBatch);
        for (std::size_t j = 0; j < kBatch; ++j) {
            batch.push_back(package_of(300, static_cast<std::uint8_t>(i)));
        }
        harness_->delivery().publish_key_packages(alice_, device_, std::move(batch),
                                                  answers[i].callback());
    }
    release_when_waiting(kPublishers);
    await_all(answers);

    std::set<std::size_t> totals;
    std::size_t full = 0;
    for (const auto& a : answers) {
        if (a.get()) {
            totals.insert(*a.get());
        } else {
            EXPECT_EQ(a.get().error(), E2eeError::Full);
            ++full;
        }
    }
    // Serialised on the device lock, each accepted batch saw the ones before it.
    EXPECT_EQ(totals, (std::set<std::size_t>{20, 40, 60, 80, 100}));
    EXPECT_EQ(full, 3U);
    EXPECT_EQ(packages_left(), "100");
}

TEST_F(E2eeRaceTest, DeregistrationAmidFetchesAndPublishesLeavesNothingToHandOut) {
    stock(10);
    hold_device();
    std::vector<Answer<FetchedKeyPackage>> fetches(16);
    std::vector<Answer<std::size_t>> publishes(8);
    Answer<void> retired;
    for (std::size_t i = 0; i < 8; ++i) {
        harness_->delivery().fetch_key_package(alice_, device_, fetches[i].callback());
        harness_->delivery().publish_key_packages(
            alice_, device_, {package_of(300, static_cast<std::uint8_t>(100 + i))},
            publishes[i].callback());
    }
    harness_->registry().deregister_device(alice_, device_, retired.callback());
    for (std::size_t i = 8; i < fetches.size(); ++i) {
        harness_->delivery().fetch_key_package(alice_, device_, fetches[i].callback());
    }
    release_when_waiting(fetches.size() + publishes.size() + 1);
    await_all(fetches);
    await_all(publishes);
    ASSERT_TRUE(ulw::test::await(harness_->reactor(), retired));

    std::set<KeyPackageBytes> handed_out;
    for (const auto& a : fetches) {
        if (a.get()) {
            EXPECT_TRUE(handed_out.insert(a.get()->package).second) << "handed out twice";
        } else {
            EXPECT_TRUE(a.get().error() == E2eeError::Revoked ||
                        a.get().error() == E2eeError::Exhausted)
                << to_string(a.get().error());
        }
    }
    for (const auto& a : publishes) {
        if (!a.get()) {
            EXPECT_EQ(a.get().error(), E2eeError::Revoked);
        }
    }
    // Whatever was published before the device was retired went with it.
    EXPECT_EQ(packages_left(), "0");
    Answer<FetchedKeyPackage> after;
    harness_->delivery().fetch_key_package(alice_, device_, after.callback());
    EXPECT_EQ(ulw::test::await(harness_->reactor(), after).error(), E2eeError::Revoked);
}

TEST_F(E2eeRaceTest, ConcurrentRegistrationsNeverPassTheDeviceCap) {
    // SetUp registered one device; the lock on the user is held while the rest queue.
    constexpr std::size_t kRegistrations = 24;
    gate_.emplace(harness_->db().session());
    ASSERT_TRUE(gate_->exec("BEGIN"));
    ASSERT_TRUE(gate_->exec("SELECT pg_advisory_xact_lock(3, hashtext($1))",
                            Params{}.add_text(alice_.view())));
    std::vector<Answer<void>> answers(kRegistrations);
    for (auto& a : answers) {
        harness_->registry().register_device(alice_, core::DeviceId::generate(clock_, random_),
                                             a.callback());
    }
    release_when_waiting(kRegistrations);
    await_all(answers);

    const auto won =
        std::ranges::count_if(answers, [](const auto& a) { return a.get().has_value(); });
    EXPECT_EQ(static_cast<std::size_t>(won), core::ports::kMaxDevicesPerUser - 1);
    for (const auto& a : answers) {
        if (!a.get()) {
            EXPECT_EQ(a.get().error(), E2eeError::Full);
        }
    }
    auto conn = harness_->db().session();
    EXPECT_EQ(scalar(conn, "SELECT count(*) FROM devices WHERE revoked_at IS NULL"),
              std::to_string(core::ports::kMaxDevicesPerUser));
}

TEST_F(E2eeRaceTest, ConcurrentCommitsForOneEpochHaveExactlyOneWinner) {
    const auto room = core::RoomId::generate(clock_, random_);
    hold_device();
    std::vector<Answer<void>> answers(kContenders);
    for (std::size_t i = 0; i < answers.size(); ++i) {
        harness_->delivery().submit_commit(room, alice_, device_, 0,
                                           ulw::test::commit_body(0, static_cast<std::uint8_t>(i)),
                                           answers[i].callback());
    }
    release_when_waiting(kContenders);
    await_all(answers);

    const auto won =
        std::ranges::count_if(answers, [](const auto& a) { return a.get().has_value(); });
    EXPECT_EQ(won, 1);
    for (const auto& a : answers) {
        if (!a.get()) {
            EXPECT_EQ(a.get().error(), E2eeError::StaleEpoch);
        }
    }
    auto conn = harness_->db().session();
    EXPECT_EQ(scalar(conn, "SELECT count(*) FROM mls_epochs WHERE room_id = $1",
                     Params{}.add_uuid(room.uuid())),
              "1");
    // The row holds the winner's commit, not a loser's.
    Answer<std::vector<core::ports::StoredCommit>> stored;
    harness_->delivery().fetch_commits(room, 0, stored.callback());
    const auto page = ulw::test::await(harness_->reactor(), stored);
    ASSERT_TRUE(page);
    ASSERT_EQ(page->size(), 1U);
    std::size_t winner = answers.size();
    for (std::size_t i = 0; i < answers.size(); ++i) {
        if (answers[i].get()) {
            winner = i;
        }
    }
    ASSERT_LT(winner, answers.size());
    EXPECT_EQ(page->front().commit, ulw::test::commit_body(0, static_cast<std::uint8_t>(winner)));
}

} // namespace
