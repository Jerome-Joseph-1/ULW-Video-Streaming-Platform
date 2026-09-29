#include "codec/ws/utf8.hpp"

#include "wire.hpp"

#include <array>
#include <cstddef>
#include <gtest/gtest.h>
#include <span>

namespace {

using codec::ws::is_valid_utf8;
using codec::ws::Utf8Validator;
using ulw::test::bytes;
using ulw::test::Bytes;
using ulw::test::text;

TEST(Utf8, AcceptsEachSequenceLengthUpToTheLastCodePoint) {
    EXPECT_TRUE(is_valid_utf8(text("plain ascii")));
    EXPECT_TRUE(is_valid_utf8(bytes({0xC2, 0x80})));             // U+0080
    EXPECT_TRUE(is_valid_utf8(bytes({0xE0, 0xA0, 0x80})));       // U+0800
    EXPECT_TRUE(is_valid_utf8(bytes({0xED, 0x9F, 0xBF})));       // U+D7FF
    EXPECT_TRUE(is_valid_utf8(bytes({0xEE, 0x80, 0x80})));       // U+E000
    EXPECT_TRUE(is_valid_utf8(bytes({0xF0, 0x90, 0x80, 0x80}))); // U+10000
    EXPECT_TRUE(is_valid_utf8(bytes({0xF4, 0x8F, 0xBF, 0xBF}))); // U+10FFFF
    EXPECT_TRUE(is_valid_utf8({}));
}

TEST(Utf8, RefusesOverlongSurrogateAndOutOfRangeForms) {
    EXPECT_FALSE(is_valid_utf8(bytes({0xC0, 0x80})));
    EXPECT_FALSE(is_valid_utf8(bytes({0xC1, 0xBF})));
    EXPECT_FALSE(is_valid_utf8(bytes({0xE0, 0x9F, 0xBF})));
    EXPECT_FALSE(is_valid_utf8(bytes({0xED, 0xA0, 0x80})));
    EXPECT_FALSE(is_valid_utf8(bytes({0xF0, 0x8F, 0xBF, 0xBF})));
    EXPECT_FALSE(is_valid_utf8(bytes({0xF4, 0x90, 0x80, 0x80})));
    EXPECT_FALSE(is_valid_utf8(bytes({0xF5, 0x80, 0x80, 0x80})));
    EXPECT_FALSE(is_valid_utf8(bytes({0xFF})));
    EXPECT_FALSE(is_valid_utf8(bytes({0x80})));
}

TEST(Utf8, RefusesATruncatedSequenceOnlyAtTheEnd) {
    Utf8Validator v;

    EXPECT_TRUE(v.feed(bytes({0xF0, 0x9F})));
    EXPECT_FALSE(v.at_boundary());
    EXPECT_TRUE(v.feed(bytes({0x98, 0x80})));
    EXPECT_TRUE(v.at_boundary());
    EXPECT_FALSE(is_valid_utf8(bytes({'a', 0xE2, 0x82})));
}

TEST(Utf8, FailsOnTheFirstByteThatCannotContinue) {
    // U+D800 is ruled out by its second byte; the third is never needed.
    Utf8Validator v;

    EXPECT_TRUE(v.feed(bytes({'x', 0xED})));
    EXPECT_FALSE(v.feed(bytes({0xA0})));
}

TEST(Utf8, GivesTheSameAnswerFedOneByteAtATime) {
    struct Sample {
        Bytes bytes;
        bool valid;
    };
    const std::array<Sample, 3> samples{{
        {.bytes = text("h\xC3\xA9llo \xE2\x82\xAC \xF0\x9F\x98\x80"), .valid = true},
        {.bytes = bytes({0xCE, 0xBA, 0xE1, 0xBD, 0xB9, 0xCF, 0x83, 0xED, 0xA0, 0x80}),
         .valid = false},
        {.bytes = bytes({0xF4, 0x8F, 0xBF}), .valid = false},
    }};
    for (const Sample& s : samples) {
        Utf8Validator v;
        bool ok = true;
        for (const std::byte b : s.bytes) {
            ok = ok && v.feed(std::span{&b, 1});
        }
        EXPECT_EQ(ok && v.at_boundary(), s.valid);
        EXPECT_EQ(is_valid_utf8(s.bytes), s.valid);
    }
}

} // namespace
