#include "infra/webpush/endpoint.hpp"
#include "net/ip_address.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <gtest/gtest.h>
#include <string>

namespace {

using namespace infra::webpush;

const PushHosts& hosts() {
    static const PushHosts defaults = PushHosts::defaults();
    return defaults;
}

EndpointError refusal(std::string_view url, const PushHosts& allowed = hosts()) {
    const auto r = check_endpoint(url, allowed);
    EXPECT_FALSE(r.has_value()) << url;
    return r ? EndpointError::Malformed : r.error();
}

TEST(PushHosts, TheDefaultsNameTheMajorBrowsersPushServices) {
    EXPECT_EQ(hosts().entries().size(), 4U);
    EXPECT_TRUE(hosts().allows("fcm.googleapis.com"));
    EXPECT_TRUE(hosts().allows("updates.push.services.mozilla.com"));
    EXPECT_TRUE(hosts().allows("web.push.apple.com"));
    EXPECT_TRUE(hosts().allows("wns2-par02p.notify.windows.com"));
    EXPECT_TRUE(hosts().allows("FCM.GoogleAPIs.com"));
    EXPECT_FALSE(hosts().allows("notify.windows.com"));
    EXPECT_FALSE(hosts().allows("evilnotify.windows.com"));
    EXPECT_FALSE(hosts().allows("fcm.googleapis.com.evil.example"));
    EXPECT_FALSE(hosts().allows("googleapis.com"));
}

TEST(PushHosts, ParsesAnOperatorsList) {
    const auto list = PushHosts::parse(" push.example.com ,\t*.push.example.net,Other.Example ");
    ASSERT_TRUE(list.has_value());
    EXPECT_EQ(list->entries(), (std::vector<std::string>{"push.example.com", "*.push.example.net",
                                                         "other.example"}));
    EXPECT_TRUE(list->allows("a.b.push.example.net"));
    EXPECT_TRUE(list->allows("other.example"));
    EXPECT_FALSE(list->allows("push.example.net"));
}

TEST(PushHosts, RefusesWhatIsNotAHostList) {
    for (const std::string_view bad :
         {"", " ", "a.com,,b.com", "*.com", "*", "*.", "a..com", "-a.com", "a-.com", "a.com.",
          "https://a.com", "a.com:443", "a.com/x", "127.0.0.1", "10.1", "a_b.com", "*.*.a.com",
          "a.0x7f"}) {
        EXPECT_FALSE(PushHosts::parse(bad).has_value()) << bad;
    }
    std::string many;
    for (int i = 0; i <= 64; ++i) {
        many += "h" + std::to_string(i) + ".example,";
    }
    EXPECT_FALSE(PushHosts::parse(many).has_value());
    EXPECT_FALSE(PushHosts::parse(std::string(64, 'a') + ".com").has_value());
}

TEST(Endpoint, AcceptsTheMajorServicesEndpoints) {
    const auto fcm =
        check_endpoint("https://fcm.googleapis.com/fcm/send/dXpB:APA91bH-xyz_123", hosts());
    ASSERT_TRUE(fcm.has_value());
    EXPECT_EQ(fcm->origin, "https://fcm.googleapis.com");
    EXPECT_EQ(fcm->url, "https://fcm.googleapis.com/fcm/send/dXpB:APA91bH-xyz_123");
    const auto wns = check_endpoint(
        "https://WNS2-BL2P.notify.windows.com:443/w/?token=BQYAAABa%2bc/d+e=", hosts());
    ASSERT_TRUE(wns.has_value());
    EXPECT_EQ(wns->origin, "https://wns2-bl2p.notify.windows.com");
    EXPECT_TRUE(check_endpoint("https://web.push.apple.com/QAbc-123", hosts()));
    EXPECT_TRUE(check_endpoint("HTTPS://updates.push.services.mozilla.com/wpush/v2/gAAA", hosts()));
}

TEST(Endpoint, RefusesAnythingButHttpsOn443) {
    EXPECT_EQ(refusal("http://fcm.googleapis.com/fcm/send/x"), EndpointError::NotHttps);
    EXPECT_EQ(refusal("ftp://fcm.googleapis.com/x"), EndpointError::NotHttps);
    EXPECT_EQ(refusal("https:/fcm.googleapis.com/x"), EndpointError::NotHttps);
    EXPECT_EQ(refusal("https://fcm.googleapis.com:8443/x"), EndpointError::NotHttps);
    EXPECT_EQ(refusal("https://fcm.googleapis.com:/x"), EndpointError::NotHttps);
    EXPECT_EQ(refusal("https://fcm.googleapis.com:0443/x"), EndpointError::NotHttps);
    EXPECT_EQ(refusal(""), EndpointError::NotHttps);
}

TEST(Endpoint, RefusesHostsTheOperatorDidNotAllow) {
    EXPECT_EQ(refusal("https://push.example.com/x"), EndpointError::HostNotAllowed);
    EXPECT_EQ(refusal("https://fcm.googleapis.com.evil.example/x"), EndpointError::HostNotAllowed);
    EXPECT_EQ(refusal("https://localhost/x"), EndpointError::HostNotAllowed);
    EXPECT_EQ(refusal("https://metadata.google.internal/computeMetadata/v1/"),
              EndpointError::HostNotAllowed);
    const auto own = PushHosts::parse("push.example.com");
    ASSERT_TRUE(own.has_value());
    EXPECT_TRUE(check_endpoint("https://push.example.com/x", *own));
    EXPECT_EQ(refusal("https://fcm.googleapis.com/x", *own), EndpointError::HostNotAllowed);
}

TEST(Endpoint, RefusesAddressLiteralsWhateverTheList) {
    for (const std::string_view url :
         {"https://127.0.0.1/x", "https://10.0.0.1/x", "https://169.254.169.254/latest/",
          "https://[::1]/x", "https://[fe80::1%25eth0]/x", "https://[::ffff:127.0.0.1]/x",
          "https://2130706433/x", "https://127.1/x", "https://0x7f.0.0.1/x", "https://0177.0.0.1/x",
          "https://1.2.3.4:443/x"}) {
        EXPECT_EQ(refusal(url), EndpointError::AddressLiteral) << url;
    }
}

TEST(Endpoint, RefusesWhatAUrlMustNotCarry) {
    for (const std::string_view url :
         {"https://fcm.googleapis.com", "https://user@fcm.googleapis.com/x",
          "https://user:pw@fcm.googleapis.com/x", "https://fcm.googleapis.com/x#frag",
          "https://fcm.googleapis.com/a b", "https://fcm.googleapis.com/a\r\nX-Evil: 1",
          "https://fcm.googleapis.com/a\\b", "https://fcm.googleapis.com/\x7f",
          "https://fcm.googleapis.com/\xc3\xa9", "https://fcm.googleapis.com\\@evil.example/x",
          "https://fcm..googleapis.com/x", "https://.fcm.googleapis.com/x", "https:///x",
          "https://fcm.googleapis.com./x", "https://fcm%2egoogleapis.com/x"}) {
        EXPECT_EQ(refusal(url), EndpointError::Malformed) << url;
    }
}

TEST(Endpoint, RefusesAnEndpointPastTheLengthLimit) {
    const std::string base = "https://fcm.googleapis.com/";
    const std::string at_limit = base + std::string(kMaxEndpointBytes - base.size(), 'a');
    EXPECT_TRUE(check_endpoint(at_limit, hosts()));
    EXPECT_EQ(refusal(at_limit + "a"), EndpointError::TooLong);
}

TEST(Endpoint, NamesEachRefusal) {
    EXPECT_EQ(to_string(EndpointError::TooLong), "too_long");
    EXPECT_EQ(to_string(EndpointError::NotHttps), "not_https");
    EXPECT_EQ(to_string(EndpointError::Malformed), "malformed");
    EXPECT_EQ(to_string(EndpointError::AddressLiteral), "address_literal");
    EXPECT_EQ(to_string(EndpointError::HostNotAllowed), "host_not_allowed");
}

bool global(std::string_view text) {
    const auto ip = net::IpAddress::parse(text);
    EXPECT_TRUE(ip.has_value()) << text;
    return ip && net::is_global_unicast(*ip);
}

TEST(GlobalUnicast, RefusesEveryAddressAStrangerCouldNotReach) {
    for (const std::string_view addr : {"0.0.0.0",
                                        "0.1.2.3",
                                        "10.0.0.1",
                                        "10.255.255.255",
                                        "100.64.0.1",
                                        "100.127.255.255",
                                        "127.0.0.1",
                                        "127.255.0.1",
                                        "169.254.169.254",
                                        "172.16.0.1",
                                        "172.31.255.255",
                                        "192.0.0.8",
                                        "192.0.2.1",
                                        "192.88.99.1",
                                        "192.168.1.1",
                                        "198.18.0.1",
                                        "198.19.255.255",
                                        "198.51.100.7",
                                        "203.0.113.9",
                                        "224.0.0.1",
                                        "239.255.255.250",
                                        "240.0.0.1",
                                        "255.255.255.255",
                                        "::",
                                        "::1",
                                        "::ffff:127.0.0.1",
                                        "::ffff:10.0.0.1",
                                        "::127.0.0.1",
                                        "64:ff9b::a00:1",
                                        "fc00::1",
                                        "fd12:3456::1",
                                        "fe80::1",
                                        "fec0::1",
                                        "ff02::1",
                                        "100::1",
                                        "2001::1",
                                        "2001:0:4136:e378::1",
                                        "2001:db8::1",
                                        "2002:a00:1::1",
                                        "3fff::1"}) {
        EXPECT_FALSE(global(addr)) << addr;
    }
}

TEST(GlobalUnicast, AcceptsPublicAddresses) {
    for (const std::string_view addr :
         {"8.8.8.8", "142.250.80.10", "1.1.1.1", "100.63.255.255", "100.128.0.1", "172.15.255.255",
          "172.32.0.1", "192.169.0.1", "198.17.255.255", "198.20.0.1", "223.255.255.255",
          "::ffff:8.8.8.8", "2607:f8b0:4004:c07::5f", "2a00:1450:4001::1", "2001:4860::8888",
          "2003::1", "3ffe::1"}) {
        EXPECT_TRUE(global(addr)) << addr;
    }
}

// Socket addresses go through the generic sockaddr header, as the kernel's interface has it.
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
TEST(GlobalUnicast, ReadsSocketAddressesOfBothFamilies) {
    sockaddr_in v4{};
    v4.sin_family = AF_INET;
    v4.sin_addr.s_addr = htonl(0x7f000001);
    const auto loop = net::address_of(reinterpret_cast<const sockaddr*>(&v4), sizeof v4);
    ASSERT_TRUE(loop.has_value());
    EXPECT_TRUE(loop->is_v4());
    EXPECT_FALSE(net::is_global_unicast(*loop));
    EXPECT_FALSE(net::address_of(reinterpret_cast<const sockaddr*>(&v4), sizeof v4 - 1));
    sockaddr_in6 v6{};
    v6.sin6_family = AF_INET6;
    v6.sin6_addr.s6_addr[0] = 0x26;
    v6.sin6_addr.s6_addr[15] = 1;
    const auto pub = net::address_of(reinterpret_cast<const sockaddr*>(&v6), sizeof v6);
    ASSERT_TRUE(pub.has_value());
    EXPECT_TRUE(net::is_global_unicast(*pub));
    sockaddr unix_family{};
    unix_family.sa_family = AF_UNIX;
    EXPECT_FALSE(net::address_of(&unix_family, sizeof unix_family));
    EXPECT_FALSE(net::address_of(nullptr, 0));
}
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

} // namespace

namespace {

TEST(Endpoint, AnyPortIsForATestPushServiceOnly) {
    const auto local = infra::webpush::PushHosts::parse("localhost");
    ASSERT_TRUE(local.has_value());
    EXPECT_FALSE(infra::webpush::check_endpoint("https://localhost:8443/p", *local));
    const auto any = infra::webpush::check_endpoint("https://localhost:8443/p", *local, true);
    ASSERT_TRUE(any.has_value());
    EXPECT_EQ(any->origin, "https://localhost:8443");
    EXPECT_EQ(infra::webpush::check_endpoint("https://localhost:443/p", *local, true)->origin,
              "https://localhost");
    for (const std::string_view bad : {"https://localhost:/p", "https://localhost:08443/p",
                                       "https://localhost:123456/p", "https://localhost:8a/p"}) {
        EXPECT_EQ(infra::webpush::check_endpoint(bad, *local, true).error(),
                  infra::webpush::EndpointError::NotHttps)
            << bad;
    }
}

} // namespace
