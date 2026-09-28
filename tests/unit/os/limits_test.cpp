#include "os/limits.hpp"

#include <sys/resource.h>

#include <cstddef>
#include <gtest/gtest.h>

namespace {

// Small enough for any hard limit a test host runs with, and far above the handful of
// descriptors a test process holds, so moving the soft limit around it breaks nothing.
constexpr std::size_t kLow = 256;
constexpr std::size_t kCap = 512;
constexpr std::size_t kHigh = 1024;

class NofileLimitTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &saved_), 0);
        if (saved_.rlim_max < kHigh) {
            GTEST_SKIP() << "hard RLIMIT_NOFILE " << saved_.rlim_max << " is below " << kHigh;
        }
    }

    void TearDown() override { ::setrlimit(RLIMIT_NOFILE, &saved_); }

    void set_soft(std::size_t soft) {
        rlimit lim = saved_;
        lim.rlim_cur = soft;
        ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &lim), 0);
    }

    static std::size_t soft_now() {
        rlimit lim{};
        ::getrlimit(RLIMIT_NOFILE, &lim);
        return lim.rlim_cur;
    }

    rlimit saved_{};
};

TEST_F(NofileLimitTest, SoftLimitBelowTheCapRisesToIt) {
    set_soft(kLow);
    const auto limits = os::raise_nofile_limit(kCap);
    ASSERT_TRUE(limits);
    EXPECT_EQ(limits->soft, kCap);
    EXPECT_EQ(limits->hard, saved_.rlim_max);
    EXPECT_EQ(soft_now(), kCap);
}

TEST_F(NofileLimitTest, SoftLimitAboveTheCapComesDownToIt) {
    set_soft(kHigh);
    const auto limits = os::raise_nofile_limit(kCap);
    ASSERT_TRUE(limits);
    EXPECT_EQ(limits->soft, kCap);
    EXPECT_EQ(soft_now(), kCap);
}

// An unprivileged process may not raise its hard limit, so asking for more must settle for it.
TEST_F(NofileLimitTest, CapAboveTheHardLimitStopsAtTheHardLimit) {
    if (saved_.rlim_max == RLIM_INFINITY) {
        GTEST_SKIP() << "no hard limit to ask past";
    }
    set_soft(kLow);
    const auto limits = os::raise_nofile_limit(saved_.rlim_max + 1);
    ASSERT_TRUE(limits);
    EXPECT_EQ(limits->soft, saved_.rlim_max);
    EXPECT_EQ(soft_now(), saved_.rlim_max);
}

} // namespace
