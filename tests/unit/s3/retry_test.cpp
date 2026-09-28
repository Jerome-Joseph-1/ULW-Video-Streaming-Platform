#include "core/ports/storage.hpp"
#include "core/util/time.hpp"
#include "infra/s3util/retry.hpp"

#include "support/fake_random.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using core::Millis;
using core::Seconds;
using core::ports::StorageError;
using infra::s3util::is_retryable;
using infra::s3util::parse_retry_after;
using infra::s3util::RetryPolicy;
using ulw::test::FakeRandom;

constexpr RetryPolicy::Config kConfig{.base = Millis(100), .cap = Millis(10'000), .max_retries = 8};

constexpr std::array kFinal{StorageError::NotFound,           StorageError::AlreadyExists,
                            StorageError::PreconditionFailed, StorageError::Unauthorized,
                            StorageError::Permanent,          StorageError::Corrupt};

// A missing delay is recorded as Millis::max() so that every range check below fails on it.
std::vector<Millis> draws(const RetryPolicy& policy, std::uint32_t retries_done, FakeRandom& random,
                          std::size_t count) {
    std::vector<Millis> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto delay =
            policy.next_delay(retries_done, StorageError::Transient, std::nullopt, random);
        EXPECT_TRUE(delay.has_value());
        out.push_back(delay.value_or(Millis::max()));
    }
    return out;
}

TEST(RetryPolicy, OnlyTransientAndThrottledFailuresAreRetryable) {
    EXPECT_TRUE(is_retryable(StorageError::Transient));
    EXPECT_TRUE(is_retryable(StorageError::Throttled));
    for (const auto e : kFinal) {
        EXPECT_FALSE(is_retryable(e));
    }
}

TEST(RetryPolicy, NeverRetriesAFinalFailureEvenWhenAskedToWait) {
    const RetryPolicy policy(kConfig);
    FakeRandom random;
    for (const auto e : kFinal) {
        EXPECT_EQ(policy.next_delay(0, e, std::nullopt, random), std::nullopt);
        EXPECT_EQ(policy.next_delay(0, e, Seconds(1), random), std::nullopt);
    }
    EXPECT_NE(policy.next_delay(0, StorageError::Throttled, std::nullopt, random), std::nullopt);
}

TEST(RetryPolicy, GivesUpAfterTheConfiguredNumberOfRetries) {
    const RetryPolicy policy(kConfig);
    FakeRandom random;
    EXPECT_NE(policy.next_delay(7, StorageError::Transient, std::nullopt, random), std::nullopt);
    EXPECT_EQ(policy.next_delay(8, StorageError::Transient, std::nullopt, random), std::nullopt);
    EXPECT_EQ(policy.next_delay(UINT32_MAX, StorageError::Transient, std::nullopt, random),
              std::nullopt);
}

TEST(RetryPolicy, CeilingDoublesFromTheBaseUntilItReachesTheCap) {
    const RetryPolicy policy(
        {.base = Millis(100), .cap = Millis(10'000), .max_retries = UINT32_MAX});
    FakeRandom random(7);
    Millis previous_ceiling{0};
    for (std::uint32_t n = 0; n < 12; ++n) {
        const Millis ceiling = std::min(Millis(100 << n), Millis(10'000));
        const auto sample = draws(policy, n, random, 200);
        const Millis longest = *std::ranges::max_element(sample);
        EXPECT_LE(longest, ceiling) << n;
        // 200 uniform draws all landing below the previous ceiling would mean it never grew.
        if (ceiling > previous_ceiling && previous_ceiling > Millis(0)) {
            EXPECT_GT(longest, previous_ceiling) << n;
        }
        previous_ceiling = ceiling;
    }
    const auto far_out = draws(policy, 1'000'000, random, 200);
    EXPECT_LE(*std::ranges::max_element(far_out), Millis(10'000));
}

TEST(RetryPolicy, JitterSpansTheWholeRangeFromZero) {
    const RetryPolicy policy(kConfig);
    FakeRandom random(3);
    const auto sample = draws(policy, 5, random, 1000);
    const auto [shortest, longest] = std::ranges::minmax(sample);
    EXPECT_LT(shortest, Millis(160));
    EXPECT_GT(longest, Millis(3040));
    EXPECT_LE(longest, Millis(3200));
}

TEST(RetryPolicy, IsDeterministicForAGivenRandomSource) {
    const RetryPolicy policy(kConfig);
    FakeRandom a(42);
    FakeRandom b(42);
    FakeRandom c(43);
    const auto first = draws(policy, 4, a, 20);
    EXPECT_EQ(first, draws(policy, 4, b, 20));
    EXPECT_NE(first, draws(policy, 4, c, 20));
}

TEST(RetryPolicy, TreatsRetryAfterAsAFloor) {
    const RetryPolicy policy(kConfig);
    FakeRandom random;
    EXPECT_EQ(policy.next_delay(0, StorageError::Throttled, Seconds(5), random), Millis(5'000));
    EXPECT_EQ(policy.next_delay(0, StorageError::Throttled, Seconds(10), random), Millis(10'000));
    EXPECT_LE(
        policy.next_delay(3, StorageError::Throttled, Seconds(0), random).value_or(Millis::max()),
        Millis(800));
}

TEST(RetryPolicy, GivesUpWhenRetryAfterExceedsTheCap) {
    const RetryPolicy policy(kConfig);
    FakeRandom random;
    EXPECT_EQ(policy.next_delay(0, StorageError::Throttled, Seconds(11), random), std::nullopt);
}

TEST(RetryPolicy, RefusesAConfigurationThatCannotBackOff) {
    EXPECT_THROW(RetryPolicy({.base = Millis(0), .cap = Millis(1), .max_retries = 1}),
                 std::invalid_argument);
    EXPECT_THROW(RetryPolicy({.base = Millis(-1), .cap = Millis(1), .max_retries = 1}),
                 std::invalid_argument);
    EXPECT_THROW(RetryPolicy({.base = Millis(200), .cap = Millis(100), .max_retries = 1}),
                 std::invalid_argument);
    EXPECT_NO_THROW(RetryPolicy({.base = Millis(100), .cap = Millis(100), .max_retries = 1}));
}

TEST(ParseRetryAfter, ReadsDeltaSecondsAndIgnoresEverythingElse) {
    EXPECT_EQ(parse_retry_after("0"), Seconds(0));
    EXPECT_EQ(parse_retry_after("120"), Seconds(120));
    EXPECT_EQ(parse_retry_after("4294967295"), Seconds(4'294'967'295));
    for (const std::string_view bad : {"", "4294967296", "-1", "+1", "1.5", " 1", "1 ", "0x10",
                                       "Wed, 21 Oct 2015 07:28:00 GMT"}) {
        EXPECT_EQ(parse_retry_after(bad), std::nullopt) << bad;
    }
}

} // namespace
