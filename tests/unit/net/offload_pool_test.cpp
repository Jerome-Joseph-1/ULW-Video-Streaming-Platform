#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"

#include "support/reactor_harness.hpp"

#include <atomic>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace {

using ulw::test::pump_until;

struct Job final : net::IOffloadJob {
    std::thread::id ran_on;
    std::thread::id completed_on;
    std::atomic<int> runs{0};
    int completions = 0;
    int value = 0;
    int result = 0;
    void run() noexcept override {
        ran_on = std::this_thread::get_id();
        ++runs;
        result = value * 2;
    }
    void complete() noexcept override {
        completed_on = std::this_thread::get_id();
        ++completions;
    }
};

class OffloadPoolTest : public ::testing::TestWithParam<net::ReactorKind> {
protected:
    void SetUp() override {
        auto r = net::make_reactor(GetParam(), clock, 1024);
        ASSERT_TRUE(r);
        reactor = std::move(*r);
        auto p = net::OffloadPool::create(*reactor, 4);
        ASSERT_TRUE(p);
        pool = std::move(*p);
    }
    void TearDown() override {
        pool.reset();
        reactor.reset();
    }

    os::SystemClock clock;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<net::OffloadPool> pool;
};

TEST_P(OffloadPoolTest, RunsOffTheLoopAndCompletesOnIt) {
    Job job;
    job.value = 21;
    pool->submit(job);
    EXPECT_EQ(pool->in_flight(), 1U);
    ASSERT_TRUE(pump_until(*reactor, [&] { return job.completions == 1; }));
    EXPECT_NE(job.ran_on, std::this_thread::get_id());
    EXPECT_EQ(job.completed_on, std::this_thread::get_id());
    EXPECT_EQ(job.result, 42);
    EXPECT_EQ(pool->in_flight(), 0U);
}

TEST_P(OffloadPoolTest, EveryJobRunsAndCompletesExactlyOnce) {
    std::vector<Job> jobs(2000);
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        jobs[i].value = static_cast<int>(i);
        pool->submit(jobs[i]);
    }
    ASSERT_TRUE(pump_until(*reactor, [&] { return pool->in_flight() == 0; }));
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        EXPECT_EQ(jobs[i].runs.load(), 1);
        EXPECT_EQ(jobs[i].completions, 1);
        EXPECT_EQ(jobs[i].result, static_cast<int>(2 * i));
    }
}

INSTANTIATE_TEST_SUITE_P(Reactors, OffloadPoolTest,
                         ::testing::Values(net::ReactorKind::IoUring, net::ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
