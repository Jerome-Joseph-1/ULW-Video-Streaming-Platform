#include "os/system_clock.hpp"

#include "fakes.hpp"
#include "heartbeat.hpp"
#include "lease_keeper.hpp"
#include "support/memory_log.hpp"
#include "support/temp_dir.hpp"

#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <optional>
#include <stop_token>
#include <string>

namespace {

using std::chrono::milliseconds;
using ulw::test::FakeQueue;
using ulw::test::Journal;
using worker::LeaseKeeper;

constexpr core::ports::JobLease kLease{.job = core::ports::JobId{7}, .fence = 2};
constexpr auto kPatience = std::chrono::seconds(10);

core::NodeId node() {
    return *core::NodeId::parse("worker-a");
}

// What the keepers under test log; each test reads only the lines it caused.
struct TestLog {
    os::SystemClock clock;
    ulw::test::MemoryLog lines;
    ops::Logger log{lines, clock, "worker", ops::Level::Debug};
};

std::size_t count(const Journal& journal, const std::string& event) {
    const auto events = journal.events();
    return static_cast<std::size_t>(std::ranges::count(events, event));
}

TEST(LeaseKeeper, BeatsEveryIntervalWhileTheJobRuns) {
    Journal journal;
    TestLog logs;
    FakeQueue queue(journal, "lease");
    const std::stop_source abandon;
    const LeaseKeeper keeper(queue, logs.log, node(), kLease,
                             {.heartbeat = milliseconds(1), .progress = std::chrono::hours(1)},
                             abandon);
    // The third beat can only come after the first two were answered as held.
    std::size_t beats = 0;
    EXPECT_TRUE(journal.wait_for(
        [&](const std::string& e) { return e == "lease heartbeat" && ++beats >= 3; }, kPatience));
    EXPECT_FALSE(keeper.lost());
    EXPECT_FALSE(abandon.stop_requested());
}

// The pod's liveness probe reads this file: it must stay fresh while a job runs, including
// while the database is out of reach, which is no reason to restart the worker.
TEST(LeaseKeeper, TouchesTheHeartbeatOnEveryBeatEvenWithTheDatabaseUnreachable) {
    const ulw::test::TempDir dir;
    const worker::Heartbeat heartbeat(dir.path() / "heartbeat");
    Journal journal;
    TestLog logs;
    FakeQueue queue(journal, "lease");
    queue.answer_heartbeat(std::nullopt);
    const std::stop_source abandon;
    const LeaseKeeper keeper(queue, logs.log, node(), kLease,
                             {.heartbeat = milliseconds(1), .progress = std::chrono::hours(1)},
                             abandon, &heartbeat);
    EXPECT_TRUE(
        journal.wait_for([](const std::string& e) { return e == "lease heartbeat"; }, kPatience));
    EXPECT_TRUE(std::filesystem::is_regular_file(heartbeat.file()));
    EXPECT_FALSE(keeper.lost());
}

TEST(LeaseKeeper, AHeartbeatMatchingNoRowAbandonsTheJob) {
    Journal journal;
    TestLog logs;
    FakeQueue queue(journal, "lease");
    queue.answer_heartbeat(false);
    const std::stop_source abandon;
    const LeaseKeeper keeper(queue, logs.log, node(), kLease,
                             {.heartbeat = milliseconds(1), .progress = std::chrono::hours(1)},
                             abandon);
    std::mutex m;
    std::condition_variable_any cv;
    std::unique_lock lock(m);
    const std::stop_token token = abandon.get_token();
    cv.wait_for(lock, token, kPatience, [] { return false; });
    EXPECT_TRUE(abandon.stop_requested());
    EXPECT_TRUE(keeper.lost());
    // Once lost, the keeper stops: one refused beat, and no more.
    EXPECT_EQ(count(journal, "lease heartbeat"), 1U);
}

TEST(LeaseKeeper, AProgressWriteMatchingNoRowAlsoAbandonsTheJob) {
    Journal journal;
    TestLog logs;
    FakeQueue queue(journal, "lease");
    queue.answer_writes(false);
    const std::stop_source abandon;
    LeaseKeeper keeper(queue, logs.log, node(), kLease,
                       {.heartbeat = std::chrono::hours(1), .progress = milliseconds(1)}, abandon);
    keeper.report(10);
    EXPECT_TRUE(
        journal.wait_for([](const std::string& e) { return e == "lease progress 10"; }, kPatience));
    std::mutex m;
    std::condition_variable_any cv;
    std::unique_lock lock(m);
    const std::stop_token token = abandon.get_token();
    cv.wait_for(lock, token, kPatience, [] { return false; });
    EXPECT_TRUE(keeper.lost());
}

TEST(LeaseKeeper, WritesProgressOnlyWhenItChanged) {
    Journal journal;
    TestLog logs;
    FakeQueue queue(journal, "lease");
    const std::stop_source abandon;
    LeaseKeeper keeper(queue, logs.log, node(), kLease,
                       {.heartbeat = std::chrono::hours(1), .progress = milliseconds(1)}, abandon);
    keeper.report(40);
    ASSERT_TRUE(
        journal.wait_for([](const std::string& e) { return e == "lease progress 40"; }, kPatience));
    keeper.report(41);
    ASSERT_TRUE(
        journal.wait_for([](const std::string& e) { return e == "lease progress 41"; }, kPatience));
    // Many ticks passed between the two, each seeing 40 again.
    EXPECT_EQ(count(journal, "lease progress 40"), 1U);
}

TEST(LeaseKeeper, AnUnreachableDatabaseIsNotALostLease) {
    Journal journal;
    TestLog logs;
    FakeQueue queue(journal, "lease");
    queue.answer_heartbeat(std::nullopt);
    const std::stop_source abandon;
    const LeaseKeeper keeper(queue, logs.log, node(), kLease,
                             {.heartbeat = milliseconds(1), .progress = std::chrono::hours(1)},
                             abandon);
    std::size_t beats = 0;
    EXPECT_TRUE(journal.wait_for(
        [&](const std::string& e) { return e == "lease heartbeat" && ++beats >= 3; }, kPatience));
    EXPECT_FALSE(keeper.lost());
    EXPECT_FALSE(abandon.stop_requested());
}

TEST(LeaseKeeper, AProgressValueTheDatabaseRefusesIsReportedOnceAndNotResent) {
    Journal journal;
    TestLog logs;
    FakeQueue queue(journal, "lease");
    queue.refuse("progress", core::ports::JobQueueError::Invalid);
    const std::stop_source abandon;
    LeaseKeeper keeper(queue, logs.log, node(), kLease,
                       {.heartbeat = std::chrono::hours(1), .progress = milliseconds(1)}, abandon);
    keeper.report(40);
    ASSERT_TRUE(
        journal.wait_for([](const std::string& e) { return e == "lease progress 40"; }, kPatience));
    keeper.report(41);
    ASSERT_TRUE(
        journal.wait_for([](const std::string& e) { return e == "lease progress 41"; }, kPatience));
    EXPECT_EQ(count(journal, "lease progress 40"), 1U);
    EXPECT_FALSE(keeper.lost());
    const auto errors = logs.lines.events("job queue call failed");
    ASSERT_GE(errors.size(), 1U);
    EXPECT_NE(errors[0].find(R"("level":"error")"), std::string::npos) << errors[0];
    EXPECT_NE(errors[0].find(R"("call":"progress","error":"invalid")"), std::string::npos);
}

} // namespace
