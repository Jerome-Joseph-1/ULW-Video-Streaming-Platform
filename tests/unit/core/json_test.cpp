#include "core/util/json.hpp"

#include <chrono>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <utility>

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
    EXPECT_EQ(v.error().offset, 20U); // just past the second "sub"
}

// Keys are compared after unescaping, so spelling one differently does not make it a new key.
TEST(Json, RejectsDuplicatesSpelledWithEscapes) {
    for (const std::string_view s :
         {R"({"a":1,"a":2})", R"({"/":1,"\/":2})", R"({"a":1,"\u0061":2})"}) {
        const auto v = parse(s);
        ASSERT_FALSE(v) << s;
        EXPECT_EQ(v.error().reason, "duplicate key") << s;
        EXPECT_EQ(v.error().offset, s.find(":2")) << s;
    }
}

// Where a key repeats more than once, the error is at the first repeat in the text, as it was
// when each key was looked up on arrival.
TEST(Json, ReportsTheFirstRepeatInTextOrder) {
    const std::string_view s = R"({"b":1,"a":2,"a":3,"b":4})";
    const auto v = parse(s);
    ASSERT_FALSE(v);
    EXPECT_EQ(v.error().offset, s.find(":3"));
}

// Duplicates are looked for when the object closes, so a later syntax error in the same object
// is the one reported.
TEST(Json, ASyntaxErrorAfterADuplicateIsReportedFirst) {
    const auto v = parse(R"({"a":1,"a":2,"b":})");
    ASSERT_FALSE(v);
    EXPECT_NE(v.error().reason, "duplicate key");
}

TEST(Json, KeepsMembersInTextOrder) {
    const auto v = parse(R"({"z":1,"a":2,"m":3})");
    ASSERT_TRUE(v);
    const auto* members = v->as_object();
    ASSERT_NE(members, nullptr);
    ASSERT_EQ(members->size(), 3U);
    EXPECT_EQ((*members)[0].first, "z");
    EXPECT_EQ((*members)[1].first, "a");
    EXPECT_EQ((*members)[2].first, "m");
    for (const auto& [key, want] : {std::pair{"z", 1}, {"a", 2}, {"m", 3}}) {
        const auto* found = v->find(key);
        ASSERT_NE(found, nullptr) << key;
        EXPECT_EQ(found->as_i64(), want) << key;
    }
}

TEST(Json, AcceptsSiblingObjectsThatShareAKey) {
    const auto v = parse(R"({"a":{"x":1},"b":{"x":2}})");
    ASSERT_TRUE(v);
    EXPECT_EQ(v->find("a")->find("x")->as_i64(), 1);
    EXPECT_EQ(v->find("b")->find("x")->as_i64(), 2);
}

TEST(Json, RejectsADuplicateKeyFarFromItsTwin) {
    std::string doc = "{\"dup\":0";
    for (int i = 0; i < 1000; ++i) {
        doc += ",\"k" + std::to_string(i) + "\":0";
    }
    const auto clean = parse(doc + "}");
    ASSERT_TRUE(clean);
    EXPECT_EQ(clean->as_object()->size(), 1001U);
    const auto twice = parse(doc + ",\"dup\":1}");
    ASSERT_FALSE(twice);
    EXPECT_EQ(twice.error().reason, "duplicate key");
    EXPECT_EQ(twice.error().offset, doc.size() + 6); // just past the second "dup"
    const auto nested = parse(R"({"a":{"x":1,"y":2,"x":3},"b":0})");
    ASSERT_FALSE(nested);
    EXPECT_EQ(nested.error().reason, "duplicate key");
    EXPECT_EQ(nested.error().offset, 21U); // just past the inner second "x"
}

// The duplicate check must not be quadratic: a peer chooses how many keys an object holds, and
// every parse runs on a reactor thread. A megabyte of distinct keys (about 110,000) took 40 s
// when each key was looked up among those before it; sorted, it is milliseconds, and at most
// about 2.5 s under the sanitizers.
TEST(Json, ManyKeysCostNoMoreThanASort) {
    constexpr std::size_t kBytes = std::size_t{1} << 20U;
    std::string doc = "{";
    for (int i = 0; doc.size() < kBytes - 16; ++i) {
        doc += (i == 0 ? "\"" : ",\"") + std::to_string(i) + "\":0";
    }
    doc += "}";
    const auto started = std::chrono::steady_clock::now();
    const auto v = parse(doc, {.max_depth = 32, .max_bytes = kBytes});
    const auto took = std::chrono::steady_clock::now() - started;
    ASSERT_TRUE(v);
    EXPECT_GT(v->as_object()->size(), 100'000U);
    EXPECT_LT(took, std::chrono::seconds(10));
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

// A string is read into room for its written length: escapes that decode shorter (or, for
// \u, longer than one byte) and a backslash at the very end still read as they should.
TEST(Json, ReadsStringsOfEveryLengthAndEscapeMix) {
    EXPECT_EQ(parse(R"("")")->as_string(), "");
    EXPECT_EQ(parse(R"("\\")")->as_string(), "\\");
    EXPECT_EQ(parse(R"("a\"b")")->as_string(), "a\"b");
    EXPECT_EQ(parse(R"("\u00e9\u20ac\ud83d\ude00")")->as_string(),
              "\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80");
    const std::string long_text(std::size_t{20} * 1024, 'x');
    const auto doc = parse(R"({"a":")" + long_text + R"(\n","b":"\")" + long_text + R"("})");
    ASSERT_TRUE(doc);
    EXPECT_EQ(doc->find("a")->as_string(), long_text + "\n");
    EXPECT_EQ(doc->find("b")->as_string(), "\"" + long_text);
    for (const std::string_view s : {R"("abc\)", R"("\)", R"("abc\")", R"(")"}) {
        EXPECT_FALSE(parse(s)) << s;
    }
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
