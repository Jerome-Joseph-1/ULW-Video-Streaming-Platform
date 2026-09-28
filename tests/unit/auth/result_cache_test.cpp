#include "core/models/ids.hpp"
#include "core/ports/auth.hpp"
#include "core/util/time.hpp"

#include "result_cache.hpp"
#include "support/fake_clock.hpp"

#include <chrono>
#include <cstddef>
#include <gtest/gtest.h>
#include <optional>
#include <string>

namespace {

using infra::auth::detail::digest_token;
using infra::auth::detail::ResultCache;
using infra::auth::detail::TokenDigest;

core::ports::Claims claims_for(const std::string& subject, core::WallTime expires_at) {
    return {.subject = *core::UserId::parse(subject),
            .email = subject + "@example.com",
            .expires_at = expires_at};
}

// Digests that land in one set: the set index comes from the first two bytes.
TokenDigest same_set(unsigned char tag) {
    TokenDigest d{};
    d[0] = 0x12;
    d[1] = 0x34;
    d[31] = tag;
    return d;
}

class ResultCacheTest : public ::testing::Test {
protected:
    ulw::test::FakeClock clock_;
    ResultCache cache_;
    core::WallTime exp_ = clock_.wall_now() + std::chrono::hours(1);
    core::MonoTime until_ = clock_.now() + std::chrono::minutes(15);
};

TEST_F(ResultCacheTest, DigestIsSha256OfTheToken) {
    const auto digest = digest_token("abc");
    ASSERT_TRUE(digest.has_value());
    // FIPS 180-2 appendix B.1.
    EXPECT_EQ((*digest)[0], 0xBA);
    EXPECT_EQ((*digest)[1], 0x78);
    EXPECT_EQ((*digest)[31], 0xAD);
}

TEST_F(ResultCacheTest, AnswersForTheSameDigestOnly) {
    const TokenDigest a = *digest_token("token-a");
    const TokenDigest b = *digest_token("token-b");
    cache_.insert(a, claims_for("alice", exp_), until_);
    const auto hit = cache_.find(a, clock_.now(), clock_.wall_now());
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->subject.view(), "alice");
    EXPECT_EQ(hit->email, "alice@example.com");
    EXPECT_FALSE(cache_.find(b, clock_.now(), clock_.wall_now()).has_value());
}

TEST_F(ResultCacheTest, AnEntryLapsesAtItsDeadline) {
    const TokenDigest a = *digest_token("token-a");
    cache_.insert(a, claims_for("alice", exp_), until_);
    EXPECT_TRUE(cache_.find(a, until_ - std::chrono::milliseconds(1), clock_.wall_now()));
    EXPECT_FALSE(cache_.find(a, until_, clock_.wall_now()));
    // Dropped, not merely hidden.
    EXPECT_FALSE(cache_.find(a, clock_.now(), clock_.wall_now()));
}

TEST_F(ResultCacheTest, AnEntryLapsesWhenTheTokenExpiresEvenBeforeItsDeadline) {
    const TokenDigest a = *digest_token("token-a");
    cache_.insert(a, claims_for("alice", clock_.wall_now() + std::chrono::minutes(2)), until_);
    const core::WallTime last_valid =
        clock_.wall_now() + std::chrono::minutes(3) - std::chrono::milliseconds(1);
    EXPECT_TRUE(cache_.find(a, clock_.now(), last_valid));
    EXPECT_FALSE(cache_.find(a, clock_.now(), last_valid + std::chrono::milliseconds(1)));
}

TEST_F(ResultCacheTest, AFullSetEvictsTheEntryDueSoonest) {
    for (std::size_t i = 0; i < ResultCache::kWays; ++i) {
        cache_.insert(same_set(static_cast<unsigned char>(i)),
                      claims_for("user" + std::to_string(i), exp_),
                      until_ + core::Seconds(static_cast<core::Seconds::rep>(i)));
    }
    cache_.insert(same_set(0xFF), claims_for("late", exp_), until_ + std::chrono::minutes(1));
    EXPECT_FALSE(cache_.find(same_set(0), clock_.now(), clock_.wall_now()));
    for (std::size_t i = 1; i < ResultCache::kWays; ++i) {
        EXPECT_TRUE(
            cache_.find(same_set(static_cast<unsigned char>(i)), clock_.now(), clock_.wall_now()))
            << i;
    }
    EXPECT_TRUE(cache_.find(same_set(0xFF), clock_.now(), clock_.wall_now()));
}

TEST_F(ResultCacheTest, EntriesInOtherSetsAreNotEvicted) {
    const TokenDigest elsewhere = *digest_token("token-elsewhere");
    ASSERT_FALSE(elsewhere[0] == 0x12 && elsewhere[1] == 0x34);
    cache_.insert(elsewhere, claims_for("alice", exp_), until_);
    for (std::size_t i = 0; i < 4 * ResultCache::kWays; ++i) {
        cache_.insert(same_set(static_cast<unsigned char>(i)), claims_for("bob", exp_), until_);
    }
    EXPECT_TRUE(cache_.find(elsewhere, clock_.now(), clock_.wall_now()));
}

TEST_F(ResultCacheTest, ClearDropsEverything) {
    const TokenDigest a = *digest_token("token-a");
    cache_.insert(a, claims_for("alice", exp_), until_);
    cache_.clear();
    EXPECT_FALSE(cache_.find(a, clock_.now(), clock_.wall_now()));
}

} // namespace
