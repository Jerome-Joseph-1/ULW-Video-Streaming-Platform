#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"

#include "job_queue.hpp"
#include "support/reactor_harness.hpp"

#include <atomic>
#include <gtest/gtest.h>
#include <latch>
#include <stop_token>
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

// Holds a pool thread until the test lets it go.
struct GateJob final : net::IOffloadJob {
    std::latch started{1};
    std::latch release{1};
    std::atomic<bool> finished{false};
    void run() noexcept override {
        started.count_down();
        release.wait();
        finished = true;
    }
    void complete() noexcept override {}
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

TEST_P(OffloadPoolTest, DestructorLetsARunningJobFinish) {
    GateJob running;
    pool->submit(running);
    running.started.wait();
    std::jthread teardown([&] { pool.reset(); });
    running.release.count_down();
    teardown.join();
    EXPECT_TRUE(running.finished);
}

TEST(OffloadQueue, StopLeavesQueuedJobsUntaken) {
    net::detail::JobQueue queue;
    Job first;
    Job second;
    queue.push(first);
    queue.push(second);
    const std::stop_source stop;
    EXPECT_EQ(queue.pop(stop.get_token()), &first);
    stop.request_stop();
    EXPECT_EQ(queue.pop(stop.get_token()), nullptr);
}

TEST(OffloadQueue, StopReleasesAnIdleWorker) {
    net::detail::JobQueue queue;
    const std::stop_source stop;
    Job never;
    net::IOffloadJob* taken = &never;
    // pop() leaves the stop wakeup to its caller, as the pool's workers register it.
    std::jthread worker([&] {
        const std::stop_callback wake_on_stop(stop.get_token(), [&queue] { queue.wake_all(); });
        taken = queue.pop(stop.get_token());
    });
    stop.request_stop();
    worker.join();
    EXPECT_EQ(taken, nullptr);
}

INSTANTIATE_TEST_SUITE_P(Reactors, OffloadPoolTest,
                         ::testing::Values(net::ReactorKind::IoUring, net::ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
