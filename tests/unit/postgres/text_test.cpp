#include "result.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <optional>

namespace {

using infra::postgres::parse_bool;
using infra::postgres::parse_int64;
using infra::postgres::parse_uint64;

TEST(ParseInt64, ReadsSignedDecimalAcrossTheWholeRange) {
    EXPECT_EQ(parse_int64("-42"), -42);
    EXPECT_EQ(parse_int64("0"), 0);
    EXPECT_EQ(parse_int64("9223372036854775807"), std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(parse_int64("-9223372036854775808"), std::numeric_limits<std::int64_t>::min());
}

TEST(ParseInt64, RejectsJunkAroundTheNumber) {
    EXPECT_EQ(parse_int64(""), std::nullopt);
    EXPECT_EQ(parse_int64("12x"), std::nullopt);
    EXPECT_EQ(parse_int64(" 12"), std::nullopt);
    EXPECT_EQ(parse_int64("12 "), std::nullopt);
    EXPECT_EQ(parse_int64("+12"), std::nullopt);
    EXPECT_EQ(parse_int64("1.5"), std::nullopt);
}

TEST(ParseInt64, RejectsValuesOutOfRange) {
    EXPECT_EQ(parse_int64("9223372036854775808"), std::nullopt);
    EXPECT_EQ(parse_int64("-9223372036854775809"), std::nullopt);
}

TEST(ParseUint64, RejectsNegativeAndOverflowingValues) {
    EXPECT_EQ(parse_uint64("18446744073709551615"), std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(parse_uint64("18446744073709551616"), std::nullopt);
    EXPECT_EQ(parse_uint64("-1"), std::nullopt);
}

TEST(ParseBool, AcceptsOnlyTheServersTextForm) {
    EXPECT_EQ(parse_bool("t"), true);
    EXPECT_EQ(parse_bool("f"), false);
    EXPECT_EQ(parse_bool("true"), std::nullopt);
    EXPECT_EQ(parse_bool(""), std::nullopt);
}

} // namespace
