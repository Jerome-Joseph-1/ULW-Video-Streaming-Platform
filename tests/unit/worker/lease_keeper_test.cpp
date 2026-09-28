#include "fakes.hpp"
#include "lease_keeper.hpp"

#include <chrono>
#include <gtest/gtest.h>
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

std::size_t count(const Journal& journal, const std::string& event) {
    const auto events = journal.events();
    return static_cast<std::size_t>(std::ranges::count(events, event));
}

TEST(LeaseKeeper, BeatsEveryIntervalWhileTheJobRuns) {
    Journal journal;
    FakeQueue queue(journal, "lease");
    const std::stop_source abandon;
    const LeaseKeeper keeper(queue, node(), kLease,
                             {.heartbeat = milliseconds(1), .progress = std::chrono::hours(1)},
                             abandon);
    // The third beat can only come after the first two were answered as held.
    std::size_t beats = 0;
    EXPECT_TRUE(journal.wait_for(
        [&](const std::string& e) { return e == "lease heartbeat" && ++beats >= 3; }, kPatience));
    EXPECT_FALSE(keeper.lost());
    EXPECT_FALSE(abandon.stop_requested());
}

TEST(LeaseKeeper, AHeartbeatMatchingNoRowAbandonsTheJob) {
    Journal journal;
    FakeQueue queue(journal, "lease");
    queue.answer_heartbeat(false);
    const std::stop_source abandon;
    const LeaseKeeper keeper(queue, node(), kLease,
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
    FakeQueue queue(journal, "lease");
    queue.answer_writes(false);
    const std::stop_source abandon;
    LeaseKeeper keeper(queue, node(), kLease,
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
    FakeQueue queue(journal, "lease");
    const std::stop_source abandon;
    LeaseKeeper keeper(queue, node(), kLease,
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
    FakeQueue queue(journal, "lease");
    queue.answer_heartbeat(std::nullopt);
    const std::stop_source abandon;
    const LeaseKeeper keeper(queue, node(), kLease,
                             {.heartbeat = milliseconds(1), .progress = std::chrono::hours(1)},
                             abandon);
    std::size_t beats = 0;
    EXPECT_TRUE(journal.wait_for(
        [&](const std::string& e) { return e == "lease heartbeat" && ++beats >= 3; }, kPatience));
    EXPECT_FALSE(keeper.lost());
    EXPECT_FALSE(abandon.stop_requested());
}

} // namespace
