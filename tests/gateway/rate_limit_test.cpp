#include "bounded_table.hpp"
#include "rate_limit.hpp"

#include <chrono>
#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

using gateway::BoundedTable;
using gateway::BucketRule;
using gateway::TokenBucket;
using namespace std::chrono_literals;

const core::MonoTime kStart{std::chrono::hours(1)};

TEST(TokenBucket, StartsFullAndRefusesOnceSpent) {
    const BucketRule rule{.burst = 3, .per_second = 1};
    TokenBucket b(rule, kStart);
    EXPECT_TRUE(b.take(rule, kStart));
    EXPECT_TRUE(b.take(rule, kStart));
    EXPECT_TRUE(b.take(rule, kStart));
    const auto refused = b.take(rule, kStart);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error(), 1000ms);
}

TEST(TokenBucket, RefillsWithElapsedTimeUpToTheBurstOnly) {
    const BucketRule rule{.burst = 2, .per_second = 4};
    TokenBucket b(rule, kStart);
    ASSERT_TRUE(b.take(rule, kStart, 2));
    // A quarter second brings back one token, not more.
    EXPECT_FALSE(b.take(rule, kStart + 200ms));
    EXPECT_TRUE(b.take(rule, kStart + 250ms));
    // A long idle saves up the burst and nothing past it.
    EXPECT_TRUE(b.take(rule, kStart + 1h, 2));
    EXPECT_FALSE(b.take(rule, kStart + 1h));
}

TEST(TokenBucket, ARefusalTakesNothingAndSaysHowLongTheWholeAmountTakes) {
    // 100 bytes a day, as the upload quota counts them.
    const BucketRule rule{.burst = 100, .per_second = 100.0 / 86'400};
    TokenBucket b(rule, kStart);
    ASSERT_TRUE(b.take(rule, kStart, 60));
    const auto refused = b.take(rule, kStart, 50);
    ASSERT_FALSE(refused);
    // Ten bytes short at 100 a day is 8,640 s.
    EXPECT_EQ(refused.error(), 8'640'000ms);
    // Nothing was taken by the refusal: 40 remain.
    EXPECT_TRUE(b.take(rule, kStart, 40));
}

TEST(TokenBucket, AClockThatStepsBackRefillsNothing) {
    const BucketRule rule{.burst = 1, .per_second = 1};
    TokenBucket b(rule, kStart + 10s);
    ASSERT_TRUE(b.take(rule, kStart + 10s));
    EXPECT_FALSE(b.take(rule, kStart));
    EXPECT_TRUE(b.take(rule, kStart + 11s));
}

TEST(RetryAfter, RoundsUpToWholeSecondsAndNeverSaysZero) {
    EXPECT_EQ(gateway::retry_after(0ms), 1s);
    EXPECT_EQ(gateway::retry_after(1ms), 1s);
    EXPECT_EQ(gateway::retry_after(1000ms), 1s);
    EXPECT_EQ(gateway::retry_after(1001ms), 2s);
    EXPECT_EQ(gateway::retry_after(30'000ms), 30s);
}

TEST(SeededHash, DependsOnTheSeed) {
    const std::string key = "203.0.113.7";
    const auto bytes = std::as_bytes(std::span(key));
    EXPECT_EQ(gateway::SeededHash(1)(bytes), gateway::SeededHash(1)(bytes));
    EXPECT_NE(gateway::SeededHash(1)(bytes), gateway::SeededHash(2)(bytes));
}

// Every key in one bucket of the index: the probing, not the hash, has to find them.
struct Collide {
    std::uint64_t operator()(int /*key*/) const noexcept { return 7; }
};
struct Spread {
    std::uint64_t operator()(int key) const noexcept { return static_cast<std::uint64_t>(key); }
};

template <class Hash> std::optional<int> value_of(BoundedTable<int, int, Hash>& t, int key) {
    bool made = false;
    const auto slot = t.acquire(key, [&] {
        made = true;
        return -1;
    });
    if (!slot || made) {
        return std::nullopt;
    }
    return t.at(*slot);
}

TEST(BoundedTable, FindsWhatItHoldsAndMakesWhatItDoesNot) {
    BoundedTable<int, int, Spread> t(4, Spread{});
    const auto a = t.acquire(10, [] { return 100; });
    const auto b = t.acquire(11, [] { return 110; });
    ASSERT_TRUE(a && b);
    EXPECT_NE(*a, *b);
    EXPECT_EQ(value_of(t, 10), 100);
    EXPECT_EQ(value_of(t, 11), 110);
    EXPECT_EQ(t.size(), 2U);
    EXPECT_EQ(t.evictions(), 0U);
}

TEST(BoundedTable, FullItDropsTheLeastRecentlyUsed) {
    BoundedTable<int, int, Spread> t(3, Spread{});
    for (int k = 1; k <= 3; ++k) {
        ASSERT_TRUE(t.acquire(k, [k] { return k; }));
    }
    // Touching 1 leaves 2 the least recently used.
    ASSERT_EQ(value_of(t, 1), 1);
    ASSERT_TRUE(t.acquire(4, [] { return 4; }));
    EXPECT_EQ(t.size(), 3U);
    EXPECT_EQ(t.evictions(), 1U);
    EXPECT_EQ(value_of(t, 1), 1);
    EXPECT_EQ(value_of(t, 3), 3);
    EXPECT_EQ(value_of(t, 4), 4);
    EXPECT_FALSE(value_of(t, 2));
}

TEST(BoundedTable, NeverDropsAPinnedEntry) {
    BoundedTable<int, int, Spread> t(2, Spread{});
    const auto held = t.acquire(1, [] { return 1; });
    ASSERT_TRUE(held);
    t.pin(*held);
    ASSERT_TRUE(t.acquire(2, [] { return 2; }));
    // 1 is older, but pinned: 2 makes way.
    ASSERT_TRUE(t.acquire(3, [] { return 3; }));
    EXPECT_EQ(t.at(*held), 1);
    EXPECT_EQ(t.pins(*held), 1U);
    EXPECT_EQ(value_of(t, 1), 1);
    EXPECT_FALSE(value_of(t, 2));
}

TEST(BoundedTable, RefusesANewKeyWhenEveryEntryIsPinned) {
    BoundedTable<int, int, Spread> t(2, Spread{});
    const auto a = t.acquire(1, [] { return 1; });
    const auto b = t.acquire(2, [] { return 2; });
    ASSERT_TRUE(a && b);
    t.pin(*a);
    t.pin(*b);
    EXPECT_FALSE(t.acquire(3, [] { return 3; }));
    // Unpinned, the entry is the most recent one, and the next to go only after the rest.
    t.unpin(*a);
    ASSERT_TRUE(t.acquire(3, [] { return 3; }));
    EXPECT_FALSE(value_of(t, 1));
    EXPECT_EQ(t.at(*b), 2);
}

TEST(BoundedTable, PinsCountSoTheLastReleaseAloneFreesTheEntry) {
    BoundedTable<int, int, Spread> t(1, Spread{});
    const auto a = t.acquire(1, [] { return 1; });
    ASSERT_TRUE(a);
    t.pin(*a);
    t.pin(*a);
    t.unpin(*a);
    EXPECT_FALSE(t.acquire(2, [] { return 2; }));
    t.unpin(*a);
    EXPECT_TRUE(t.acquire(2, [] { return 2; }));
}

TEST(BoundedTable, EvictionFromOneLongProbeRunKeepsEveryOtherKeyReachable) {
    // All keys share one home position, so each removal must close the gap it leaves or the
    // keys past it are lost.
    BoundedTable<int, int, Collide> t(8, Collide{});
    for (int k = 0; k < 8; ++k) {
        ASSERT_TRUE(t.acquire(k, [k] { return k * 10; }));
    }
    for (int k = 8; k < 40; ++k) {
        ASSERT_TRUE(t.acquire(k, [k] { return k * 10; }));
        EXPECT_EQ(t.size(), 8U);
        // The eight most recent are all still found, with their own values.
        for (int back = k - 7; back <= k; ++back) {
            // value_of would refresh each; read them oldest first so the order is kept.
            EXPECT_EQ(value_of(t, back), back * 10) << "after inserting " << k;
        }
    }
    EXPECT_EQ(t.evictions(), 32U);
}

TEST(BoundedTable, KeysWrappingPastTheEndOfTheIndexSurviveRemovals) {
    // Capacity 4 gives an index of 8; homes near the end wrap round to the front.
    struct NearEnd {
        std::uint64_t operator()(int key) const noexcept {
            return static_cast<std::uint64_t>(6 + (key % 3));
        }
    };
    BoundedTable<int, int, NearEnd> t(4, NearEnd{});
    std::vector<int> recent;
    for (int k = 0; k < 30; ++k) {
        ASSERT_TRUE(t.acquire(k, [k] { return k; }));
        recent.push_back(k);
        if (recent.size() > 4) {
            recent.erase(recent.begin());
        }
        for (const int r : recent) {
            EXPECT_EQ(value_of(t, r), r) << "after inserting " << k;
        }
    }
}

TEST(ForwardedClient, IsTheRightmostEntryNoTrustedProxyWrote) {
    const auto proxies = std::vector{*net::IpNetwork::parse("10.42.0.0/16")};
    const auto peer = *net::IpAddress::parse("10.42.3.4");
    const auto client = [&](std::vector<http::HeaderField> headers) {
        return gateway::forwarded_client(peer, headers, proxies);
    };
    const auto ip = [](std::string_view text) { return *net::IpAddress::parse(text); };

    // Envoy appends the address it was reached from.
    EXPECT_EQ(client({{"X-Forwarded-For", "198.51.100.7"}}), ip("198.51.100.7"));
    // Whatever the client put in front of that is its own invention.
    EXPECT_EQ(client({{"x-forwarded-for", "203.0.113.1, 198.51.100.7"}}), ip("198.51.100.7"));
    // A chain of trusted hops is walked through.
    EXPECT_EQ(client({{"X-Forwarded-For", "198.51.100.7,10.42.9.9 , 10.42.0.5"}}),
              ip("198.51.100.7"));
    // Two fields are one list, the later field last.
    EXPECT_EQ(client({{"X-Forwarded-For", "198.51.100.7"}, {"X-Forwarded-For", "10.42.0.5"}}),
              ip("198.51.100.7"));
    // No header: the proxy itself, a health check or a scrape.
    EXPECT_EQ(client({{"Host", "gw"}}), peer);
    // A malformed entry ends the walk at the last good one.
    EXPECT_EQ(client({{"X-Forwarded-For", "198.51.100.7, garbage, 10.42.0.5"}}), ip("10.42.0.5"));
    EXPECT_EQ(client({{"X-Forwarded-For", "198.51.100.7:443"}}), peer);
    EXPECT_EQ(client({{"X-Forwarded-For", ""}}), peer);
    // IPv6, and IPv4 in either form, are all addresses.
    EXPECT_EQ(client({{"X-Forwarded-For", "2001:db8::5"}}), ip("2001:db8::5"));
    EXPECT_EQ(client({{"X-Forwarded-For", "::ffff:198.51.100.7"}}), ip("198.51.100.7"));
}

TEST(ClientKey, CountsAnIpv6SiteAsOneClient) {
    const auto a = *net::IpAddress::parse("2001:db8:1:2::1");
    const auto b = *net::IpAddress::parse("2001:db8:1:2:ffff::9");
    const auto other = *net::IpAddress::parse("2001:db8:1:3::1");
    EXPECT_EQ(gateway::client_key(a), gateway::client_key(b));
    EXPECT_NE(gateway::client_key(a), gateway::client_key(other));
    const auto v4 = *net::IpAddress::parse("198.51.100.7");
    EXPECT_EQ(gateway::client_key(v4), v4);
    EXPECT_NE(gateway::client_key(v4), gateway::client_key(*net::IpAddress::parse("198.51.100.8")));
}

} // namespace
