#include "recent_keys.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"

#include <gtest/gtest.h>
#include <string>

namespace {

using core::Millis;

class RecentKeysTest : public ::testing::Test {
protected:
    static rt::MessageKey key(int n) { return *rt::MessageKey::parse("k" + std::to_string(n)); }

    ulw::test::FakeClock clock_;
    ulw::test::FakeRandom random_;
    const core::RoomId room_ = core::RoomId::generate(clock_, random_);
    const core::RoomId other_room_ = core::RoomId::generate(clock_, random_);
    const core::UserId alice_ = *core::UserId::parse("alice");
    const core::UserId bob_ = *core::UserId::parse("bob");
};

TEST_F(RecentKeysTest, AKeyIsTheSendersOwnInItsOwnRoom) {
    rt::RecentKeys keys(8, Millis{60'000});
    keys.remember(room_, alice_, key(1), 7, clock_.now());
    EXPECT_EQ(keys.find(room_, alice_, key(1)), 7U);
    EXPECT_FALSE(keys.find(room_, bob_, key(1)));
    EXPECT_FALSE(keys.find(other_room_, alice_, key(1)));
    EXPECT_FALSE(keys.find(room_, alice_, key(2)));
}

TEST_F(RecentKeysTest, TheFirstSeqRememberedForAKeyStays) {
    rt::RecentKeys keys(8, Millis{60'000});
    keys.remember(room_, alice_, key(1), 7, clock_.now());
    keys.remember(room_, alice_, key(1), 9, clock_.now());
    EXPECT_EQ(keys.find(room_, alice_, key(1)), 7U);
    EXPECT_EQ(keys.size(), 1U);
}

TEST_F(RecentKeysTest, PastItsCapacityTheOldestKeysGoFirst) {
    rt::RecentKeys keys(3, Millis{60'000});
    for (int n = 1; n <= 5; ++n) {
        keys.remember(room_, alice_, key(n), static_cast<std::uint64_t>(n), clock_.now());
    }
    EXPECT_EQ(keys.size(), 3U);
    EXPECT_FALSE(keys.find(room_, alice_, key(1)));
    EXPECT_FALSE(keys.find(room_, alice_, key(2)));
    EXPECT_EQ(keys.find(room_, alice_, key(3)), 3U);
    EXPECT_EQ(keys.find(room_, alice_, key(5)), 5U);
}

TEST_F(RecentKeysTest, KeysOlderThanTheWindowAreForgottenWhenTheNextIsRemembered) {
    rt::RecentKeys keys(100, Millis{60'000});
    keys.remember(room_, alice_, key(1), 1, clock_.now());
    clock_.advance(Millis{30'000});
    keys.remember(room_, alice_, key(2), 2, clock_.now());
    clock_.advance(Millis{30'001});
    keys.remember(room_, alice_, key(3), 3, clock_.now());
    EXPECT_FALSE(keys.find(room_, alice_, key(1)));
    EXPECT_EQ(keys.find(room_, alice_, key(2)), 2U);
    EXPECT_EQ(keys.find(room_, alice_, key(3)), 3U);
}

TEST_F(RecentKeysTest, AKeyIsIdCharactersOnlyAndAtMostSixtyFourOfThem) {
    EXPECT_TRUE(rt::MessageKey::parse("0f4c2a9e-5b1d-4c7e-9a3f-2d8e6b1c0a7f"));
    EXPECT_TRUE(rt::MessageKey::parse(std::string(rt::MessageKey::kMaxLength, 'Z')));
    EXPECT_FALSE(rt::MessageKey::parse(""));
    EXPECT_FALSE(rt::MessageKey::parse(std::string(rt::MessageKey::kMaxLength + 1, 'Z')));
    EXPECT_FALSE(rt::MessageKey::parse("a\"b"));
    EXPECT_FALSE(rt::MessageKey::parse("a b"));
}

} // namespace
