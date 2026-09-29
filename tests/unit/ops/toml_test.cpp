#include "ops/toml.hpp"

#include <gtest/gtest.h>
#include <string>
#include <string_view>

namespace {

using ops::toml::Kind;

std::string error_of(std::string_view doc) {
    const auto r = ops::toml::parse(doc);
    return r ? "accepted" : std::to_string(r.error().line) + ": " + r.error().reason;
}

TEST(Toml, TablesQualifyTheKeysBelowThem) {
    const auto r = ops::toml::parse("top = 1\n[listen]\nport = 8080 # comment\n"
                                    "[storage.s3]\nendpoint = \"http://m:9000\"\n");
    ASSERT_TRUE(r) << r.error().reason;
    ASSERT_EQ(r->size(), 3U);
    EXPECT_EQ((*r)[0].key, "top");
    EXPECT_EQ((*r)[1].key, "listen.port");
    EXPECT_EQ((*r)[1].kind, Kind::Integer);
    EXPECT_EQ((*r)[1].value, "8080");
    EXPECT_EQ((*r)[1].line, 3U);
    EXPECT_EQ((*r)[2].key, "storage.s3.endpoint");
    EXPECT_EQ((*r)[2].value, "http://m:9000");
}

TEST(Toml, DottedKeysAndBlanksAroundTheDotsAreOneKey) {
    const auto r = ops::toml::parse("a . b\t= true\n");
    ASSERT_TRUE(r);
    EXPECT_EQ((*r)[0].key, "a.b");
    EXPECT_EQ((*r)[0].kind, Kind::Boolean);
    EXPECT_EQ((*r)[0].value, "true");
}

TEST(Toml, BasicStringsUnescapeAndLiteralStringsDoNot) {
    const auto r = ops::toml::parse(R"(a = "x\"y\\z\t\u00e9\U0001F600")"
                                    "\n"
                                    R"(b = 'C:\path\n')");
    ASSERT_TRUE(r);
    EXPECT_EQ((*r)[0].value, "x\"y\\z\t\xC3\xA9\xF0\x9F\x98\x80");
    EXPECT_EQ((*r)[1].value, "C:\\path\\n");
}

TEST(Toml, IntegersAreNormalisedToPlainDecimal) {
    const auto r = ops::toml::parse("a = 8_388_608\nb = -3\nc = +4\nd = -0\n");
    ASSERT_TRUE(r);
    EXPECT_EQ((*r)[0].value, "8388608");
    EXPECT_EQ((*r)[1].value, "-3");
    EXPECT_EQ((*r)[2].value, "4");
    EXPECT_EQ((*r)[3].value, "0");
}

TEST(Toml, WindowsLineEndingsAreAccepted) {
    const auto r = ops::toml::parse("[a]\r\nb = 1\r\n");
    ASSERT_TRUE(r);
    EXPECT_EQ((*r)[0].key, "a.b");
}

TEST(Toml, WhatTheSubsetLeavesOutIsRefusedWithItsLine) {
    EXPECT_EQ(error_of("a = 1\nb = [1]\n"), "2: arrays are not supported");
    EXPECT_EQ(error_of("b = {c = 1}"), "1: inline tables are not supported");
    EXPECT_EQ(error_of("b = 1.5"), "1: only decimal integers, strings and booleans are supported");
    EXPECT_EQ(error_of("b = 1979-05-27"),
              "1: only decimal integers, strings and booleans are supported");
    EXPECT_EQ(error_of("b = 0x10"), "1: only decimal integers, strings and booleans are supported");
    EXPECT_EQ(error_of("b = \"\"\"\nx\n\"\"\""), "1: multi-line strings are not supported");
    EXPECT_EQ(error_of("[[b]]"), "1: arrays of tables are not supported");
    EXPECT_EQ(error_of("\"b\" = 1"), "1: quoted keys are not supported");
}

TEST(Toml, MalformedInputIsRefused) {
    EXPECT_EQ(error_of("a = \"open"), "1: unterminated string");
    EXPECT_EQ(error_of("a = 1 2"), "1: unexpected text after the value");
    EXPECT_EQ(error_of("a"), "1: expected = after the key");
    EXPECT_EQ(error_of("a ="), "1: expected a value");
    EXPECT_EQ(error_of("a = 012"), "1: leading zero in an integer");
    EXPECT_EQ(error_of("a = 1__0"), "1: only decimal integers, strings and booleans are supported");
    EXPECT_EQ(error_of("a = 9223372036854775808"), "1: integer out of range");
    EXPECT_EQ(error_of("a = \"\\q\""), "1: unknown escape in a string");
    EXPECT_EQ(error_of("a = \"\\ud800\""), "1: escape is not a unicode scalar value");
    EXPECT_EQ(error_of("a = \"x\x01\""), "1: control character in a string");
    EXPECT_EQ(error_of("a = truex"), "1: not a supported value");
    EXPECT_EQ(error_of("\xC0\x80 = 1"), "0: not UTF-8");
}

TEST(Toml, NothingMayBeDefinedTwice) {
    EXPECT_EQ(error_of("a = 1\na = 2"), "2: a defined twice");
    EXPECT_EQ(error_of("[t]\n[t]"), "2: table [t] defined twice");
    EXPECT_EQ(error_of("a = 1\na.b = 2"), "2: a is a value, not a table");
    EXPECT_EQ(error_of("a.b = 1\na = 2"), "2: a is already a table");
    EXPECT_EQ(error_of("[a]\n[b]\n[a.c]\nx = 1\n[a]"), "5: table [a] defined twice");
    EXPECT_EQ(error_of("a = 1\n[a]"), "2: a is already a value");
}

} // namespace
