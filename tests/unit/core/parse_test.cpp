#include "core/util/parse.hpp"

#include <cstdint>
#include <gtest/gtest.h>

namespace {

using core::parse_integer;

TEST(ParseInteger, AcceptsExactlyADecimalNumber) {
    EXPECT_EQ(parse_integer<std::uint64_t>("0"), 0U);
    EXPECT_EQ(parse_integer<std::uint64_t>("8388608"), 8388608U);
    EXPECT_EQ(parse_integer<std::uint64_t>("18446744073709551615"), UINT64_MAX);
    EXPECT_EQ(parse_integer<std::int64_t>("-42"), -42);
}

TEST(ParseInteger, RejectsAnythingElse) {
    for (const std::string_view s :
         {"", " 1", "1 ", "+1", "-1", "1x", "0x10", "1.0", "1e3", "18446744073709551616"}) {
        EXPECT_FALSE(parse_integer<std::uint64_t>(s)) << s;
    }
    EXPECT_FALSE(parse_integer<std::int64_t>("+1"));
    EXPECT_FALSE(parse_integer<std::uint8_t>("256"));
}

} // namespace
