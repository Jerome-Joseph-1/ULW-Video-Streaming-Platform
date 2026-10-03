#include "http/origin.hpp"

#include <array>
#include <gtest/gtest.h>
#include <string>
#include <string_view>

namespace {

// A browser writes an IPv6 host as the URL standard serialises it: lowercase hex, no leading
// zeros, the first longest run of two or more zero pieces as "::", and no dotted IPv4 tail.
// Origin is compared byte for byte, so only that spelling can ever match.
TEST(Origin, Ipv6LiteralsAsABrowserWritesThemAreOrigins) {
    for (const char* origin :
         {"https://[::1]", "http://[::1]:3000", "https://[::]", "https://[1::]",
          "https://[2001:db8::1]", "https://[::ffff:c000:201]:8443", "https://[1:2:3:4:5:6:7:8]",
          "https://[1:0:3:4:5:6:7:8]", "https://[2001:db8::1:0:0:1]", "https://[1:0:0:2::3]",
          "https://[fe80::abcd:ef01]"}) {
        EXPECT_TRUE(http::is_origin(origin)) << origin;
    }
}

// Another spelling of the same address names a host no browser sends, so a list holding it
// would trust nothing while appearing to trust that host: it is refused at load instead.
TEST(Origin, NonCanonicalIpv6LiteralsAreRefused) {
    for (const char* origin :
         {"https://[0:0:0:0:0:0:0:1]", "http://[0:0:0:0:0:0:0:1]", "http://[0::1]:3000",
          "https://[::01]", "https://[::0001]", "https://[2001:0db8::1]",
          "https://[2001:db8:0:0:0:0:0:1]", "https://[2001:db8::0:1]", "https://[1::2:0:0:0:3]",
          "https://[1::3:4:5:6:7:8]", "https://[1:2:3:4:5:6:7::]", "https://[::ffff:192.0.2.1]",
          "https://[::127.0.0.1]", "http://[::127.0.0.1]", "https://[2001:DB8::1]",
          "https://[0:0:0:0:0:0:0:0]"}) {
        EXPECT_FALSE(http::is_origin(origin)) << origin;
    }
}

TEST(Origin, MalformedIpv6LiteralsAreRefused) {
    for (const char* origin :
         {"https://[:]", "https://[:::]", "https://[1:2]", "https://[1::2::3]", "https://[12345::]",
          "https://[1:2:3:4:5:6:7:8:9]", "https://[::1:]", "https://[:1::]", "https://[1:]",
          "https://[:1]", "https://[1:2:3:4:5:6:7:8::]", "https://[::1:2:3:4:5:6:7:8]",
          "https://[1.2.3.4]", "https://[::1]x", "https://[::1]]"}) {
        EXPECT_FALSE(http::is_origin(origin)) << origin;
    }
}

// Both sides are compared as written: a configured [::1] trusts no other spelling a client
// might send, and the list itself can only hold the canonical one.
TEST(Origin, OnlyTheExactSpellingIsAllowed) {
    const auto allowed = http::parse_origin_list("https://[::1]:8443,https://[2001:db8::1]");
    ASSERT_TRUE(allowed);
    EXPECT_TRUE(http::origin_allowed(*allowed, "https://[::1]:8443"));
    EXPECT_TRUE(http::origin_allowed(*allowed, "https://[2001:db8::1]"));
    for (const std::string_view other :
         {"https://[0:0:0:0:0:0:0:1]:8443", "https://[::01]:8443", "https://[2001:DB8::1]",
          "https://[2001:0db8::1]", "https://[2001:db8:0:0:0:0:0:1]"}) {
        EXPECT_FALSE(http::origin_allowed(*allowed, other)) << other;
    }
    EXPECT_FALSE(http::parse_origin_list("https://[::1],https://[0:0:0:0:0:0:0:1]"));
}

} // namespace
