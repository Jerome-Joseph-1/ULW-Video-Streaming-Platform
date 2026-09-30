#include "infra/auth/token_extractor.hpp"

#include <expected>
#include <gtest/gtest.h>
#include <initializer_list>
#include <string_view>
#include <utility>

namespace {

using infra::auth::TokenExtractor;
using infra::auth::TokenSourceError;

using Header = std::pair<std::string_view, std::string_view>;

std::expected<std::string_view, TokenSourceError>
extract(std::initializer_list<Header> headers, std::string_view cookie_name = "auth_token") {
    TokenExtractor extractor{cookie_name};
    for (const auto& [name, value] : headers) {
        extractor.on_header(name, value);
    }
    return extractor.token();
}

std::unexpected<TokenSourceError> refused(TokenSourceError e) {
    return std::unexpected(e);
}

TEST(TokenExtractorTest, TakesTheBearerToken) {
    EXPECT_EQ(extract({{"Authorization", "Bearer aa.bb.cc"}}), "aa.bb.cc");
    EXPECT_EQ(extract({{"authorization", "bearer aa.bb.cc"}}), "aa.bb.cc");
    EXPECT_EQ(extract({{"AUTHORIZATION", "BEARER aa.bb.cc"}}), "aa.bb.cc");
    EXPECT_EQ(extract({{"Authorization", "Bearer   aa.bb.cc  "}}), "aa.bb.cc");
}

TEST(TokenExtractorTest, RefusesAnythingButOneBearerToken) {
    for (const std::string_view value :
         {"Basic dXNlcjpwYXNz", "Bearer", "Bearer ", "Bearer\taa.bb.cc", "Beareraa.bb.cc",
          "Bearer aa.bb.cc dd", "Bearer aa.bb.cc=", "Bearer aa+bb/cc", "Token aa.bb.cc", ""}) {
        EXPECT_EQ(extract({{"Authorization", value}}), refused(TokenSourceError::Malformed))
            << '"' << value << '"';
    }
}

TEST(TokenExtractorTest, TakesEveryCharacterOfTheTokenAlphabetAndNoOther) {
    constexpr std::string_view kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.";
    const std::string all = "Bearer " + std::string(kAlphabet);
    EXPECT_EQ(extract({{"Authorization", all}}), kAlphabet);
    for (char c = 0x21; c < 0x7F; ++c) {
        if (kAlphabet.contains(c)) {
            continue;
        }
        const std::string value = "Bearer aa" + std::string(1, c) + "bb";
        EXPECT_EQ(extract({{"Authorization", value}}), refused(TokenSourceError::Malformed))
            << value;
    }
}

// Only spaces and tabs are trimmed; any other control character is part of the value.
TEST(TokenExtractorTest, ControlCharactersAroundTheTokenAreNotTrimmed) {
    for (const std::string_view value : {"Bearer aa.bb.cc\x01",
                                         "\x01"
                                         "Bearer aa.bb.cc",
                                         "Bearer aa.bb.cc\x1f",
                                         "\x0b"
                                         "Bearer aa.bb.cc"}) {
        EXPECT_EQ(extract({{"Authorization", value}}), refused(TokenSourceError::Malformed));
    }
}

TEST(TokenExtractorTest, TwoAuthorizationHeadersAreAmbiguous) {
    EXPECT_EQ(extract({{"Authorization", "Bearer aa.bb.cc"}, {"authorization", "Bearer dd.ee.ff"}}),
              refused(TokenSourceError::Ambiguous));
    EXPECT_EQ(extract({{"Authorization", "Bearer aa.bb.cc"}, {"Authorization", "Bearer aa.bb.cc"}}),
              refused(TokenSourceError::Ambiguous));
}

TEST(TokenExtractorTest, TakesTheConfiguredCookie) {
    EXPECT_EQ(extract({{"Cookie", "theme=dark; auth_token=aa.bb.cc; lang=en"}}), "aa.bb.cc");
    EXPECT_EQ(extract({{"cookie", "auth_token=aa.bb.cc"}}), "aa.bb.cc");
    EXPECT_EQ(extract({{"Cookie", "flag; auth_token = aa.bb.cc ;"}}), "aa.bb.cc");
    EXPECT_EQ(extract({{"Cookie", R"(auth_token="aa.bb.cc")"}}), "aa.bb.cc");
    EXPECT_EQ(extract({{"Cookie", "a=1"}, {"Cookie", "auth_token=aa.bb.cc"}}), "aa.bb.cc");
}

TEST(TokenExtractorTest, MatchesTheCookieNameExactly) {
    EXPECT_EQ(extract({{"Cookie", "Auth_Token=aa.bb.cc"}}), refused(TokenSourceError::Missing));
    EXPECT_EQ(extract({{"Cookie", "xauth_token=aa.bb.cc"}}), refused(TokenSourceError::Missing));
    EXPECT_EQ(extract({{"Cookie", "auth_token_stage=aa.bb.cc"}}),
              refused(TokenSourceError::Missing));
    EXPECT_EQ(
        extract({{"Cookie", "auth_token=aa.bb.cc; auth_token_stage=dd.ee.ff"}}, "auth_token_stage"),
        "dd.ee.ff");
}

TEST(TokenExtractorTest, TheCookieTwiceIsAmbiguous) {
    EXPECT_EQ(extract({{"Cookie", "auth_token=aa.bb.cc; auth_token=dd.ee.ff"}}),
              refused(TokenSourceError::Ambiguous));
    EXPECT_EQ(extract({{"Cookie", "auth_token=aa.bb.cc"}, {"Cookie", "auth_token=aa.bb.cc"}}),
              refused(TokenSourceError::Ambiguous));
}

TEST(TokenExtractorTest, ACookieTokenMustLookLikeACompactJws) {
    EXPECT_EQ(extract({{"Cookie", "auth_token=aa.bb cc"}}), refused(TokenSourceError::Malformed));
    EXPECT_EQ(extract({{"Cookie", "auth_token=aa%2Ebb.cc"}}), refused(TokenSourceError::Malformed));
    EXPECT_EQ(extract({{"Cookie", R"(auth_token=")"}}), refused(TokenSourceError::Malformed));
}

TEST(TokenExtractorTest, AnEmptiedCookieIsNoToken) {
    EXPECT_EQ(extract({{"Cookie", "auth_token=; theme=dark"}}), refused(TokenSourceError::Missing));
    EXPECT_EQ(extract({{"Cookie", R"(auth_token="")"}}), refused(TokenSourceError::Missing));
}

TEST(TokenExtractorTest, TheAuthorizationHeaderWinsOverTheCookie) {
    EXPECT_EQ(extract({{"Cookie", "auth_token=aa.bb.cc"}, {"Authorization", "Bearer dd.ee.ff"}}),
              "dd.ee.ff");
}

TEST(TokenExtractorTest, AMalformedAuthorizationHeaderDoesNotFallBackToTheCookie) {
    EXPECT_EQ(extract({{"Authorization", "Basic dXNlcjpwYXNz"}, {"Cookie", "auth_token=aa.bb.cc"}}),
              refused(TokenSourceError::Malformed));
}

TEST(TokenExtractorTest, UserHeadersAreNeverAToken) {
    EXPECT_EQ(extract({{"X-User-Id", "alice"},
                       {"x-user-email", "alice@example.com"},
                       {"X-Auth-Token", "aa.bb.cc"},
                       {"Proxy-Authorization", "Bearer aa.bb.cc"}}),
              refused(TokenSourceError::Missing));
    EXPECT_EQ(extract({}), refused(TokenSourceError::Missing));
}

} // namespace
