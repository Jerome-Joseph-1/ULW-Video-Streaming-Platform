#include "infra/auth/base64url.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <gtest/gtest.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace {

using infra::auth::append_base64url;
using infra::auth::decode_base64url;
using infra::auth::encode_base64url;

TEST(Base64UrlTest, EncodesTheRfc4648VectorsWithoutPadding) {
    EXPECT_EQ(encode_base64url(std::string_view{}), "");
    EXPECT_EQ(encode_base64url(std::string_view{"f"}), "Zg");
    EXPECT_EQ(encode_base64url(std::string_view{"fo"}), "Zm8");
    EXPECT_EQ(encode_base64url(std::string_view{"foo"}), "Zm9v");
    EXPECT_EQ(encode_base64url(std::string_view{"foob"}), "Zm9vYg");
    EXPECT_EQ(encode_base64url(std::string_view{"fooba"}), "Zm9vYmE");
    EXPECT_EQ(encode_base64url(std::string_view{"foobar"}), "Zm9vYmFy");
}

TEST(Base64UrlTest, AppendsWhatEncodeReturns) {
    std::string bytes;
    for (int i = 0; i < 256; ++i) {
        bytes.push_back(static_cast<char>(255 - i));
    }
    const std::span<const unsigned char> octets{
        reinterpret_cast<const unsigned char*>(bytes.data()), // NOLINT(*-reinterpret-cast)
        bytes.size()};
    for (std::size_t len = 0; len <= 6; ++len) {
        std::string out = "prefix:";
        append_base64url(out, octets.first(len));
        EXPECT_EQ(out, "prefix:" + encode_base64url(octets.first(len))) << len;
    }
    std::string out;
    append_base64url(out, octets);
    EXPECT_EQ(out, encode_base64url(octets));
}

TEST(Base64UrlTest, UsesTheUrlSafeAlphabet) {
    constexpr std::array<unsigned char, 3> kBytes{0xFB, 0xFF, 0xBF};
    EXPECT_EQ(encode_base64url(kBytes), "-_-_");
    EXPECT_EQ(decode_base64url("-_-_"), std::string("\xFB\xFF\xBF"));
}

TEST(Base64UrlTest, RoundTripsEveryByteAtEveryLengthModuloThree) {
    std::string bytes;
    for (int i = 0; i < 256; ++i) {
        bytes.push_back(static_cast<char>(i));
    }
    for (std::size_t len = 250; len <= 256; ++len) {
        const std::string_view in{bytes.data(), len};
        EXPECT_EQ(decode_base64url(encode_base64url(in)), std::string(in)) << len;
    }
}

TEST(Base64UrlTest, RejectsPadding) {
    EXPECT_EQ(decode_base64url("Zg=="), std::nullopt);
    EXPECT_EQ(decode_base64url("Zm8="), std::nullopt);
}

TEST(Base64UrlTest, RejectsTheStandardAlphabetAndStrayCharacters) {
    EXPECT_EQ(decode_base64url("+/+/"), std::nullopt);
    EXPECT_EQ(decode_base64url("Zm9v Zg"), std::nullopt);
    EXPECT_EQ(decode_base64url("Zm9v.Zg"), std::nullopt);
    EXPECT_EQ(decode_base64url(std::string_view{"Zm9v\0Zg", 7}), std::nullopt);
}

TEST(Base64UrlTest, RejectsALoneTrailingCharacter) {
    EXPECT_EQ(decode_base64url("Zm9vY"), std::nullopt);
    EXPECT_EQ(decode_base64url("Z"), std::nullopt);
}

// "Zh" decodes to the same byte as "Zg"; accepting it would give a signature two spellings.
TEST(Base64UrlTest, RejectsNonZeroBitsAfterTheLastByte) {
    EXPECT_EQ(decode_base64url("Zg"), std::string("f"));
    EXPECT_EQ(decode_base64url("Zh"), std::nullopt);
    EXPECT_EQ(decode_base64url("Zm8"), std::string("fo"));
    EXPECT_EQ(decode_base64url("Zm9"), std::nullopt);
}

TEST(Base64UrlTest, DecodesToBytesExactlyAsToTextAndRefusesTheSame) {
    std::string bytes;
    for (int i = 0; i < 256; ++i) {
        bytes.push_back(static_cast<char>(i));
    }
    const auto decoded = infra::auth::decode_base64url_bytes(encode_base64url(bytes));
    ASSERT_TRUE(decoded);
    const auto expected = std::as_bytes(std::span{bytes});
    EXPECT_TRUE(std::ranges::equal(*decoded, expected));
    for (const std::string_view bad : {"Zg==", "+/+/", "Zm9vY", "Zh"}) {
        EXPECT_EQ(infra::auth::decode_base64url_bytes(bad), std::nullopt) << bad;
    }
}

} // namespace
