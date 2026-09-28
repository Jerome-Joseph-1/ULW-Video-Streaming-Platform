#include "core/util/json.hpp"

#include <gtest/gtest.h>
#include <string>

namespace {

using core::json::parse;
using Kind = core::json::Value::Kind;

TEST(Json, ReadsTheUploadRequestShape) {
    const auto v =
        parse(R"({"filename":"trip.mp4","size_bytes":104857600,"content_type":"video/mp4"})");
    ASSERT_TRUE(v);
    EXPECT_EQ(v->find("filename")->as_string(), "trip.mp4");
    EXPECT_EQ(v->find("size_bytes")->as_u64(), 104857600U);
    EXPECT_EQ(v->find("content_type")->as_string(), "video/mp4");
    EXPECT_EQ(v->find("missing"), nullptr);
}

TEST(Json, IntegersOnlyReadAsIntegers) {
    EXPECT_EQ(parse("18446744073709551615")->as_u64(), UINT64_MAX);
    EXPECT_FALSE(parse("18446744073709551616")->as_u64());
    EXPECT_FALSE(parse("-1")->as_u64());
    EXPECT_EQ(parse("-1")->as_i64(), -1);
    EXPECT_FALSE(parse("1.5")->as_u64());
    EXPECT_FALSE(parse("1e3")->as_u64());
    EXPECT_FALSE(parse("\"7\"")->as_u64());
}

TEST(Json, RejectsMalformedNumbers) {
    for (const std::string_view s :
         {"01", "-", "1.", ".5", "1e", "+1", "0x10", "NaN", "Infinity"}) {
        EXPECT_FALSE(parse(s)) << s;
    }
}

TEST(Json, DecodesEscapesIncludingSurrogatePairs) {
    const auto v = parse(R"("a\"b\\c\/d\né😀")");
    ASSERT_TRUE(v);
    EXPECT_EQ(v->as_string(), "a\"b\\c/d\n\xc3\xa9\xf0\x9f\x98\x80");
}

TEST(Json, RejectsLoneSurrogatesAndBadEscapes) {
    for (const std::string_view s :
         {R"("\ud83d")", R"("\ude00")", R"("\ud83dx")", R"("\x41")", R"("\u12")"}) {
        EXPECT_FALSE(parse(s)) << s;
    }
}

TEST(Json, RejectsInvalidUtf8AndRawControlCharacters) {
    EXPECT_FALSE(parse("\"\xc3\x28\""));
    EXPECT_FALSE(parse("\"\xed\xa0\x80\""));
    EXPECT_FALSE(parse("\"a\nb\""));
    EXPECT_TRUE(parse("\"\xe2\x82\xac\""));
}

TEST(Json, RejectsDuplicateKeys) {
    const auto v = parse(R"({"sub":"alice","sub":"mallory"})");
    ASSERT_FALSE(v);
    EXPECT_EQ(v.error().reason, "duplicate key");
}

TEST(Json, RejectsTrailingDataAndUnterminatedInput) {
    for (const std::string_view s :
         {"{} x", "[1,]", "[1", "{\"a\":1,}", "{\"a\" 1}", "", "tru", "\"abc", "{\"a\":1}}"}) {
        EXPECT_FALSE(parse(s)) << s;
    }
}

TEST(Json, BoundsNestingDepth) {
    // max_depth counts nested values: 32 arrays inside one another is the most allowed.
    const std::string ok = std::string(32, '[') + std::string(32, ']');
    const std::string deep = std::string(33, '[') + std::string(33, ']');
    EXPECT_TRUE(parse(ok));
    EXPECT_FALSE(parse(deep));
}

TEST(Json, BoundsDocumentSize) {
    const std::string big = "\"" + std::string(100, 'a') + "\"";
    EXPECT_FALSE(parse(big, {.max_depth = 32, .max_bytes = 64}));
}

TEST(Json, ReadsNestedStructures) {
    const auto v = parse(R"({"keys":[{"kid":"k1","n":"x"},{"kid":"k2"}],"ok":true,"x":null})");
    ASSERT_TRUE(v);
    const auto* keys = v->find("keys")->as_array();
    ASSERT_NE(keys, nullptr);
    ASSERT_EQ(keys->size(), 2U);
    EXPECT_EQ((*keys)[1].find("kid")->as_string(), "k2");
    EXPECT_EQ(v->find("ok")->as_bool(), true);
    EXPECT_EQ(v->find("x")->kind(), Kind::Null);
}

TEST(Json, EscapedStringsRoundTrip) {
    const std::string raw = "q\"b\\s\x01\x1f\n\t\xc3\xa9";
    std::string out;
    core::json::append_string(out, raw);
    EXPECT_EQ(out, R"("q\"b\\s\u0001\u001f\n\t)"
                   "\xc3\xa9\"");
    EXPECT_EQ(parse(out)->as_string(), raw);
}

} // namespace
