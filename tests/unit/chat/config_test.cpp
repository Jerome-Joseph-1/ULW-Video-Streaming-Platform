#include "config.hpp"

#include <gtest/gtest.h>
#include <map>
#include <string>
#include <vector>

namespace {

using chat::Config;

class ChatConfigTest : public ::testing::Test {
protected:
    [[nodiscard]] std::expected<Config, chat::ConfigError> load() const {
        return chat::load_config([this](std::string_view name) -> std::optional<std::string> {
            const auto it = env.find(std::string(name));
            return it == env.end() ? std::nullopt : std::optional<std::string>(it->second);
        });
    }

    [[nodiscard]] std::string refused_variable() const {
        const auto config = load();
        EXPECT_FALSE(config.has_value());
        return config ? std::string() : config.error().variable;
    }

    std::map<std::string, std::string, std::less<>> env{
        {"HOSTNAME", "chat-5d9f7c6b8-x2k4q"},
        {"ULW_NODE_ADDRESS", "10.42.0.17:9201"},
        {"ULW_NODE_SECRET", "test-only-node-secret-0123456789abcdef"},
        {"ULW_DATABASE_URL", "postgresql://ulw@db/ulw"},
        {"JWKS_URL", "https://auth.example.test/.well-known/jwks.json"},
        {"JWT_ISSUER", "https://auth.example.test"},
        {"JWT_AUDIENCE", "ulw-test-audience"},
    };
};

TEST_F(ChatConfigTest, TheMinimalProductionEnvironmentLoadsWithDefaults) {
    const auto config = load();
    ASSERT_TRUE(config) << config.error().variable << ": " << config.error().reason;
    EXPECT_EQ(config->node.view(), "chat-5d9f7c6b8-x2k4q");
    EXPECT_EQ(config->port, 9101);
    EXPECT_EQ(config->node_address, "10.42.0.17:9201");
    EXPECT_EQ(config->node_secret, "test-only-node-secret-0123456789abcdef");
    EXPECT_EQ(config->reactor, net::ReactorKind::IoUring);
    EXPECT_EQ(config->jwt_audience, "ulw-test-audience");
    EXPECT_EQ(config->jwt_subject_claim, "sub");
    EXPECT_EQ(config->auth_cookie, "auth_token");
    EXPECT_TRUE(config->allowed_origins.empty());
}

TEST_F(ChatConfigTest, CallsAreOffUnlessTheLiveKitKeyIsSetAndThenNeedAllFour) {
    // The URLs alone, as an overlay may set them where no LiveKit secret exists, turn nothing on.
    env["LIVEKIT_API_URL"] = "http://livekit:7880";
    env["LIVEKIT_CLIENT_URL"] = "wss://media.example.test";
    auto config = load();
    ASSERT_TRUE(config);
    EXPECT_FALSE(config->calls);
    env["LIVEKIT_API_KEY"] = "";
    ASSERT_TRUE(load());
    EXPECT_FALSE(load()->calls);

    env["LIVEKIT_API_KEY"] = "test-key";
    EXPECT_EQ(refused_variable(), "LIVEKIT_API_SECRET");
    env["LIVEKIT_API_SECRET"] = "test-only-livekit-secret-0123456789abcdef";
    config = load();
    ASSERT_TRUE(config) << config.error().variable;
    ASSERT_TRUE(config->calls);
    EXPECT_EQ(config->calls->api_url, "http://livekit:7880");
    EXPECT_EQ(config->calls->client_url, "wss://media.example.test");
    EXPECT_EQ(config->calls->api_key, "test-key");
    EXPECT_EQ(config->calls->api_secret, "test-only-livekit-secret-0123456789abcdef");
    for (const char* name : {"LIVEKIT_API_URL", "LIVEKIT_CLIENT_URL"}) {
        const std::string kept = env[name];
        env.erase(name);
        EXPECT_EQ(refused_variable(), name);
        env[name] = kept;
    }
}

TEST_F(ChatConfigTest, StayingRootIsAnExplicitChoiceAndTheUserToBecomeOptional) {
    const auto defaults = load();
    ASSERT_TRUE(defaults);
    EXPECT_FALSE(defaults->allow_root);
    EXPECT_TRUE(defaults->run_as_user.empty());
    env["ULW_RUN_AS_USER"] = "ulw";
    env["ULW_ALLOW_ROOT"] = "1";
    const auto set = load();
    ASSERT_TRUE(set);
    EXPECT_EQ(set->run_as_user, "ulw");
    EXPECT_TRUE(set->allow_root);
    env["ULW_ALLOW_ROOT"] = "yes";
    EXPECT_EQ(refused_variable(), "ULW_ALLOW_ROOT");
}

TEST_F(ChatConfigTest, AnExplicitNodeIdWinsOverTheHostname) {
    env["ULW_NODE_ID"] = "chat-1";
    const auto config = load();
    ASSERT_TRUE(config);
    EXPECT_EQ(config->node.view(), "chat-1");
}

TEST_F(ChatConfigTest, EachRequiredVariableIsNamedWhenMissing) {
    for (const std::string name : {"ULW_NODE_ADDRESS", "ULW_NODE_SECRET", "ULW_DATABASE_URL",
                                   "JWKS_URL", "JWT_ISSUER", "JWT_AUDIENCE"}) {
        const std::string saved = env.at(name);
        env.erase(name);
        EXPECT_EQ(refused_variable(), name);
        env[name] = saved;
    }
    env.erase("HOSTNAME");
    EXPECT_EQ(refused_variable(), "ULW_NODE_ID");
}

TEST_F(ChatConfigTest, ANodeNameMustBeALabel) {
    env["ULW_NODE_ID"] = "Chat_1";
    EXPECT_EQ(refused_variable(), "ULW_NODE_ID");
    env.erase("ULW_NODE_ID");
    env["HOSTNAME"] = "chat.example";
    EXPECT_EQ(refused_variable(), "HOSTNAME");
}

TEST_F(ChatConfigTest, TheNodeAddressMustBeNumericAndApartFromTheClientPort) {
    for (const char* bad : {"chat-0.chat:9201", "10.42.0.17", "10.42.0.17:0", "[fd00::1]"}) {
        env["ULW_NODE_ADDRESS"] = bad;
        EXPECT_EQ(refused_variable(), "ULW_NODE_ADDRESS") << bad;
    }
    env["ULW_NODE_ADDRESS"] = "[fd00::1]:9201";
    ASSERT_TRUE(load());
    env["ULW_NODE_ADDRESS"] = "10.42.0.17:9101";
    EXPECT_EQ(refused_variable(), "ULW_NODE_ADDRESS");
}

TEST_F(ChatConfigTest, TheNodeAddressMustBeOneOtherNodesCanDial) {
    for (const char* nowhere : {"0.0.0.0:9201", "[::]:9201", "127.0.0.1:9201", "[::1]:9201"}) {
        env["ULW_NODE_ADDRESS"] = nowhere;
        EXPECT_EQ(refused_variable(), "ULW_NODE_ADDRESS") << nowhere;
    }
    // A cluster on one host, as the tests run, says so.
    env["ULW_DEV_LOOPBACK_NODES"] = "1";
    env["ULW_NODE_ADDRESS"] = "127.0.0.1:9201";
    EXPECT_TRUE(load());
    env["ULW_NODE_ADDRESS"] = "0.0.0.0:9201";
    EXPECT_EQ(refused_variable(), "ULW_NODE_ADDRESS");
}

TEST_F(ChatConfigTest, ANodeSecretShorterThan32BytesIsRefused) {
    env["ULW_NODE_SECRET"] = std::string(31, 'k');
    EXPECT_EQ(refused_variable(), "ULW_NODE_SECRET");
    env["ULW_NODE_SECRET"] = std::string(32, 'k');
    EXPECT_TRUE(load());
}

TEST_F(ChatConfigTest, OutOfRangeAndMalformedValuesAreRefused) {
    const std::vector<std::pair<std::string, std::string>> bad{
        {"ULW_LISTEN_PORT", "0"},
        {"ULW_LISTEN_PORT", "65536"},
        {"ULW_LISTEN_PORT", "91o1"},
        {"ULW_REACTOR", "kqueue"},
        {"JWKS_URL", "http://auth.example.test/jwks.json"},
    };
    for (const auto& [name, value] : bad) {
        const auto saved =
            env.find(name) == env.end() ? std::nullopt : std::optional<std::string>(env[name]);
        env[name] = value;
        EXPECT_EQ(refused_variable(), name) << value;
        if (saved) {
            env[name] = *saved;
        } else {
            env.erase(name);
        }
    }
}

TEST_F(ChatConfigTest, ThePresenceGraceIsTheServicesUnlessSetInMilliseconds) {
    EXPECT_FALSE(load()->presence_grace);
    env["ULW_PRESENCE_GRACE_MS"] = "2500";
    EXPECT_EQ(load()->presence_grace, core::Millis{2'500});
    for (const char* bad : {"-1", "2.5", "600001", "10s"}) {
        env["ULW_PRESENCE_GRACE_MS"] = bad;
        EXPECT_EQ(refused_variable(), "ULW_PRESENCE_GRACE_MS") << bad;
    }
}

TEST_F(ChatConfigTest, TheRingTimeoutIsTheServicesUnlessSetInMilliseconds) {
    EXPECT_FALSE(load()->ring_timeout);
    env["ULW_CALL_RING_TIMEOUT_MS"] = "3000";
    EXPECT_EQ(load()->ring_timeout, core::Millis{3'000});
    for (const char* bad : {"-1", "999", "300001", "45s"}) {
        env["ULW_CALL_RING_TIMEOUT_MS"] = bad;
        EXPECT_EQ(refused_variable(), "ULW_CALL_RING_TIMEOUT_MS") << bad;
    }
}

TEST_F(ChatConfigTest, ADevelopmentKeySetReplacesTheJwksUrlButNotBoth) {
    env["ULW_DEV_JWKS_FILE"] = "/etc/ulw/dev-jwks.json";
    env["ULW_DEV_MODE"] = "1";
    EXPECT_EQ(refused_variable(), "JWKS_URL");
    env.erase("JWKS_URL");
    env.erase("JWT_AUDIENCE");
    const auto config = load();
    ASSERT_TRUE(config);
    EXPECT_EQ(config->dev_jwks_file, "/etc/ulw/dev-jwks.json");
    EXPECT_TRUE(config->jwks_url.empty());
    // What ulw_devtoken mints by default; a JWKS has no default (below).
    EXPECT_EQ(config->jwt_audience, "ulw-dev");
}

TEST_F(ChatConfigTest, KeysFromAJwksNeedTheAudienceSaidOutright) {
    env.erase("JWT_AUDIENCE");
    const auto config = load();
    ASSERT_FALSE(config);
    EXPECT_EQ(config.error().variable, "JWT_AUDIENCE");
    EXPECT_NE(config.error().reason.find("JWKS_URL"), std::string::npos) << config.error().reason;
}

TEST_F(ChatConfigTest, TheSubjectClaimIsSubUnlessNamed) {
    env["ULW_JWT_SUBJECT_CLAIM"] = "user_id";
    const auto named = load();
    ASSERT_TRUE(named);
    EXPECT_EQ(named->jwt_subject_claim, "user_id");
    for (const char* bad : {"user id", "sub\"", "{sub}"}) {
        env["ULW_JWT_SUBJECT_CLAIM"] = bad;
        EXPECT_EQ(refused_variable(), "ULW_JWT_SUBJECT_CLAIM") << bad;
    }
}

TEST_F(ChatConfigTest, KeysStayTrustedADayWithoutARefreshUnlessSetInHours) {
    EXPECT_EQ(load()->jwks_max_stale_hours, 24U);
    env["ULW_JWKS_MAX_STALE_HOURS"] = "6";
    EXPECT_EQ(load()->jwks_max_stale_hours, 6U);
    for (const char* bad : {"0", "169", "1.5", "24h"}) {
        env["ULW_JWKS_MAX_STALE_HOURS"] = bad;
        EXPECT_EQ(refused_variable(), "ULW_JWKS_MAX_STALE_HOURS") << bad;
    }
}

// Whoever can set it signs any identity they like, so it takes development mode said outright,
// and never in a Kubernetes pod, where every real deployment runs.
TEST_F(ChatConfigTest, ADevelopmentKeySetIsRefusedOutsideDevelopmentModeAndInAnyPod) {
    env.erase("JWKS_URL");
    env["ULW_DEV_JWKS_FILE"] = "/etc/ulw/dev-jwks.json";
    EXPECT_EQ(refused_variable(), "ULW_DEV_JWKS_FILE");
    env["ULW_DEV_MODE"] = "0";
    EXPECT_EQ(refused_variable(), "ULW_DEV_JWKS_FILE");
    env["ULW_DEV_MODE"] = "1";
    env["KUBERNETES_SERVICE_HOST"] = "10.43.0.1";
    EXPECT_EQ(refused_variable(), "ULW_DEV_JWKS_FILE");
    env.erase("KUBERNETES_SERVICE_HOST");
    EXPECT_TRUE(load());
}

TEST_F(ChatConfigTest, PerClientLimitsAreTheServicesUnlessSetAndProxiesAreCidrBlocks) {
    const auto defaults = load();
    ASSERT_TRUE(defaults);
    EXPECT_FALSE(defaults->client_limits.max_connections_per_ip);
    EXPECT_FALSE(defaults->client_limits.max_connections_per_ip_block);
    EXPECT_FALSE(defaults->client_limits.new_connections_per_ip_per_second);
    EXPECT_FALSE(defaults->client_limits.max_sessions_per_user);
    EXPECT_TRUE(defaults->client_limits.trusted_proxies.empty());
    EXPECT_EQ(defaults->client_limits.trusted_proxy_hops, 1U);
    env["ULW_MAX_CONNECTIONS_PER_IP"] = "40";
    env["ULW_MAX_CONNECTIONS_PER_IP_BLOCK"] = "200";
    env["ULW_NEW_CONNECTIONS_PER_IP_PER_SECOND"] = "5";
    env["ULW_MAX_SESSIONS_PER_USER"] = "8";
    env["ULW_TRUSTED_PROXIES"] = "10.42.0.0/16, 10.43.0.0/16";
    env["ULW_TRUSTED_PROXY_HOPS"] = "2";
    const auto set = load();
    ASSERT_TRUE(set) << set.error().variable;
    EXPECT_EQ(set->client_limits.max_connections_per_ip, 40U);
    EXPECT_EQ(set->client_limits.max_connections_per_ip_block, 200U);
    EXPECT_EQ(set->client_limits.new_connections_per_ip_per_second, 5U);
    EXPECT_EQ(set->client_limits.max_sessions_per_user, 8U);
    EXPECT_EQ(set->client_limits.trusted_proxies.size(), 2U);
    EXPECT_EQ(set->client_limits.trusted_proxy_hops, 2U);
    for (const auto& [name, bad] : std::vector<std::pair<std::string, std::string>>{
             {"ULW_MAX_CONNECTIONS_PER_IP", "0"},
             {"ULW_MAX_CONNECTIONS_PER_IP", "1281"},
             {"ULW_MAX_CONNECTIONS_PER_IP_BLOCK", "0"},
             {"ULW_MAX_CONNECTIONS_PER_IP_BLOCK", "1281"},
             {"ULW_MAX_CONNECTIONS_PER_IP_BLOCK", "eighty"},
             {"ULW_NEW_CONNECTIONS_PER_IP_PER_SECOND", "0"},
             {"ULW_MAX_SESSIONS_PER_USER", "1281"},
             {"ULW_TRUSTED_PROXIES", "0.0.0.0/0"},
             {"ULW_TRUSTED_PROXIES", "10.42.0.1/16"},
             {"ULW_TRUSTED_PROXY_HOPS", "0"}}) {
        auto changed = env;
        env[name] = bad;
        EXPECT_EQ(refused_variable(), name) << bad;
        env = changed;
    }
}

// As the gateway's: a hop count means nothing without the proxies it counts, and would
// otherwise be taken as set while every peer's own address is used.
TEST_F(ChatConfigTest, AProxyHopCountWithoutTrustedProxiesIsRefused) {
    env["ULW_TRUSTED_PROXY_HOPS"] = "2";
    EXPECT_EQ(refused_variable(), "ULW_TRUSTED_PROXY_HOPS");
    env["ULW_TRUSTED_PROXIES"] = "10.42.0.0/16";
    EXPECT_TRUE(load());
}

// As the gateway's: a block wider than an IPv4 /8 or an IPv6 /32 is rarely one's own proxies,
// and lets many peers name any client, so it is started with but warned about.
TEST_F(ChatConfigTest, ATrustedProxyBlockWiderThanASlash8OrASlash32IsWarnedAbout) {
    env["ULW_TRUSTED_PROXIES"] = "10.0.0.0/8, 10.42.0.0/16, 2001:db8::/32";
    const auto narrow = load();
    ASSERT_TRUE(narrow);
    EXPECT_TRUE(chat::wide_trusted_proxies(narrow->client_limits).empty());
    env["ULW_TRUSTED_PROXIES"] = "10.0.0.0/7, 10.42.0.0/16, 2001:db8::/31";
    const auto wide = load();
    ASSERT_TRUE(wide);
    EXPECT_EQ(chat::wide_trusted_proxies(wide->client_limits), (std::vector<unsigned>{7, 31}));
}

TEST_F(ChatConfigTest, AllowedOriginsAreExactSchemeHostAndPort) {
    env["ULW_ALLOWED_ORIGINS"] = "https://app.example.com,http://localhost:5173";
    const auto config = load();
    ASSERT_TRUE(config);
    EXPECT_EQ(config->allowed_origins,
              (std::vector<std::string>{"https://app.example.com", "http://localhost:5173"}));
    for (const char* bad : {"app.example.com", "https://app.example.com/", "https://App.test",
                            "https://a.test,,https://b.test", "https://"}) {
        env["ULW_ALLOWED_ORIGINS"] = bad;
        EXPECT_EQ(refused_variable(), "ULW_ALLOWED_ORIGINS") << bad;
    }
}

} // namespace
