#include "token_bucket.hpp"

#include <chrono>
#include <gtest/gtest.h>

namespace {

using core::Millis;
using std::chrono::microseconds;

// Any instant will do: the bucket only ever looks at differences.
const core::MonoTime kStart{std::chrono::hours(1000)};

TEST(TokenBucket, ItsWholeBurstIsThereAtOnceAndThenItSaysWhenTheNextToken) {
    chat::TokenBucket bucket(10, 2, kStart);
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(bucket.take(kStart)) << i;
    }
    // Two a second: the next whole token is half a second away.
    EXPECT_EQ(bucket.take(kStart), std::unexpected(Millis{500}));
}

TEST(TokenBucket, ItRefillsAtItsRateAndNoSooner) {
    chat::TokenBucket bucket(1, 2, kStart);
    ASSERT_TRUE(bucket.take(kStart));
    EXPECT_EQ(bucket.take(kStart + Millis{499}), std::unexpected(Millis{1}));
    EXPECT_TRUE(bucket.take(kStart + Millis{500}));
    EXPECT_EQ(bucket.take(kStart + Millis{500}), std::unexpected(Millis{500}));
}

TEST(TokenBucket, AnIdleBucketHoldsNoMoreThanItsBurst) {
    chat::TokenBucket bucket(3, 2, kStart);
    ASSERT_TRUE(bucket.take(kStart));
    const core::MonoTime later = kStart + std::chrono::hours(1);
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(bucket.take(later)) << i;
    }
    EXPECT_FALSE(bucket.take(later));
}

TEST(TokenBucket, TakesLessThanAMillisecondApartStillEarnTheirShare) {
    chat::TokenBucket bucket(1, 2, kStart);
    ASSERT_TRUE(bucket.take(kStart));
    // Every 0.7 ms, which rounds down to nothing each time: the first token comes at 500 ms
    // all the same, not never.
    core::MonoTime t = kStart;
    int attempts = 0;
    while (!bucket.take(t) && attempts < 1000) {
        t += microseconds(700);
        ++attempts;
    }
    EXPECT_GE(t - kStart, Millis{500});
    EXPECT_LT(t - kStart, Millis{501});
}

TEST(TokenBucket, ItIsFullOnlyOnceEveryTokenIsBack) {
    chat::TokenBucket bucket(2, 2, kStart);
    EXPECT_TRUE(bucket.full(kStart));
    ASSERT_TRUE(bucket.take(kStart));
    ASSERT_TRUE(bucket.take(kStart));
    EXPECT_FALSE(bucket.full(kStart + Millis{999}));
    EXPECT_TRUE(bucket.full(kStart + Millis{1'000}));
}

} // namespace
