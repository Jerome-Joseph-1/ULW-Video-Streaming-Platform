#include "core/util/url.hpp"

#include <gtest/gtest.h>
#include <string_view>

namespace {

using core::cluster_host;
using core::loopback_host;
using core::secure_url;
using core::split_url;

TEST(SplitUrl, TakesTheSchemeAndTheHostWithoutItsPort) {
    const auto plain = split_url("https://kubernetes.default.svc:443/api");
    ASSERT_TRUE(plain);
    EXPECT_EQ(plain->scheme, "https");
    EXPECT_EQ(plain->host, "kubernetes.default.svc");
    const auto v6 = split_url("http://[::1]:8080");
    ASSERT_TRUE(v6);
    EXPECT_EQ(v6->host, "[::1]");
    EXPECT_EQ(split_url("wss://media.example.test?x")->host, "media.example.test");
    for (const std::string_view bad :
         {"", "kubernetes.default.svc", "://host", "http://", "http://[::1"}) {
        EXPECT_FALSE(split_url(bad)) << bad;
    }
}

TEST(SecureUrl, APlainSchemeOnlyWhereTheHostAllowsIt) {
    EXPECT_TRUE(secure_url("https://10.43.0.1", "https", "http", loopback_host));
    EXPECT_TRUE(secure_url("http://127.0.0.1:7880", "https", "http", loopback_host));
    EXPECT_TRUE(secure_url("http://localhost", "https", "http", loopback_host));
    EXPECT_TRUE(secure_url("http://[::1]:1", "https", "http", loopback_host));
    EXPECT_FALSE(secure_url("http://10.43.0.1", "https", "http", loopback_host));
    EXPECT_FALSE(secure_url("http://127.0.0.1.example.com", "https", "http", loopback_host));
    EXPECT_FALSE(secure_url("ftp://127.0.0.1", "https", "http", loopback_host));
    EXPECT_TRUE(secure_url("http://livekit:7880", "https", "http", cluster_host));
    EXPECT_TRUE(
        secure_url("http://livekit.ulw.svc.cluster.local:7880", "https", "http", cluster_host));
    EXPECT_FALSE(secure_url("http://livekit.example.com", "https", "http", cluster_host));
    EXPECT_TRUE(secure_url("wss://media.example.test", "wss", "ws", loopback_host));
    EXPECT_FALSE(secure_url("ws://media.example.test", "wss", "ws", loopback_host));
}

} // namespace
