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

// A host whose last label is a number is an IPv4 address to the URL standard, which writes it
// as four decimal octets: 0x7f.1, 127.1, 010.0.0.1 and 10.0.0.1. are all written otherwise, and
// a.0x1 or 256.0.0.1 is no URL at all, so none of them could ever match.
TEST(Origin, Ipv4HostsAreFourDecimalOctets) {
    for (const char* origin :
         {"https://10.0.0.1", "https://192.0.2.255:8443", "https://0.0.0.0",
          "http://127.0.0.1:8080", "https://1e3", "https://a.example1", "https://1.example"}) {
        EXPECT_TRUE(http::is_origin(origin)) << origin;
    }
    for (const char* origin : {"https://010.0.0.1",
                               "https://10.0.0.01",
                               "https://10.00.0.1",
                               "https://0x7f.1",
                               "https://0x7f.0.0.1",
                               "https://127.1",
                               "https://127.0.1",
                               "https://2130706433",
                               "https://256.0.0.1",
                               "https://1.2.3.256",
                               "https://1.2.3.4.5",
                               "https://10.0.0.1.",
                               "https://10.0.0.1.:8443",
                               "https://a.0x1",
                               "https://example.123",
                               "https://a.09",
                               "https://0x",
                               "https://1..2.3",
                               "http://127.1",
                               "http://0x7f.0.0.1:3000",
                               "http://127.0.0.1."}) {
        EXPECT_FALSE(http::is_origin(origin)) << origin;
    }
}

// A browser lowercases a domain and leaves the scheme's default port out; both were already
// refused. It keeps a trailing dot (https://a.example./ is sent as https://a.example.), so a
// domain ending in one can match and is not refused.
TEST(Origin, DomainsAreLowercaseWithoutTheDefaultPort) {
    for (const char* origin : {"https://App.example", "https://a.EXAMPLE:8443",
                               "https://a.example:443", "http://localhost:80"}) {
        EXPECT_FALSE(http::is_origin(origin)) << origin;
    }
    EXPECT_TRUE(http::is_origin("https://a.example."));
    EXPECT_TRUE(http::is_origin("https://a.example.:8443"));
}

} // namespace
