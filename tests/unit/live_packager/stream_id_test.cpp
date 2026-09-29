#include "stream_id.hpp"

#include <gtest/gtest.h>
#include <string>

namespace {

TEST(StreamId, AcceptsLettersDigitsUnderscoreAndDash) {
    const auto id = live::StreamId::parse("Show_2026-09-29");
    ASSERT_TRUE(id);
    EXPECT_EQ(id->str(), "Show_2026-09-29");
    EXPECT_EQ(id->key_prefix(), "live/Show_2026-09-29/");
}

TEST(StreamId, RefusesWhatCouldClimbOutOfItsPrefixOrBreakAKey) {
    for (const char* bad : {"", "a/b", "..", "a.b", "a b", "a\n", "é", "a?x=1", "a%2f"}) {
        EXPECT_FALSE(live::StreamId::parse(bad)) << bad;
    }
}

TEST(StreamId, IsAtMost64Characters) {
    EXPECT_TRUE(live::StreamId::parse(std::string(64, 'a')));
    EXPECT_FALSE(live::StreamId::parse(std::string(65, 'a')));
}

} // namespace
