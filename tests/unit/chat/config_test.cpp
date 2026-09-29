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
        {"ULW_DATABASE_URL", "postgresql://ulw@db/ulw"},
        {"JWKS_URL", "https://auth.example.test/.well-known/jwks.json"},
        {"JWT_ISSUER", "https://auth.example.test"},
    };
};

TEST_F(ChatConfigTest, TheMinimalProductionEnvironmentLoadsWithDefaults) {
    const auto config = load();
    ASSERT_TRUE(config) << config.error().variable << ": " << config.error().reason;
    EXPECT_EQ(config->node.view(), "chat-5d9f7c6b8-x2k4q");
    EXPECT_EQ(config->port, 9101);
    EXPECT_EQ(config->node_address, "10.42.0.17:9201");
    EXPECT_EQ(config->node_port, 9201);
    EXPECT_EQ(config->reactor, net::ReactorKind::IoUring);
    EXPECT_EQ(config->jwt_audience, "askedin-platform");
    EXPECT_EQ(config->auth_cookie, "auth_token");
    EXPECT_TRUE(config->allowed_origins.empty());
}

TEST_F(ChatConfigTest, AnExplicitNodeIdWinsOverTheHostname) {
    env["ULW_NODE_ID"] = "chat-1";
    const auto config = load();
    ASSERT_TRUE(config);
    EXPECT_EQ(config->node.view(), "chat-1");
}

TEST_F(ChatConfigTest, EachRequiredVariableIsNamedWhenMissing) {
    for (const std::string name :
         {"ULW_NODE_ADDRESS", "ULW_DATABASE_URL", "JWKS_URL", "JWT_ISSUER"}) {
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
    EXPECT_EQ(load()->node_port, 9201);
    env["ULW_NODE_ADDRESS"] = "10.42.0.17:9101";
    EXPECT_EQ(refused_variable(), "ULW_NODE_ADDRESS");
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

TEST_F(ChatConfigTest, ADevelopmentKeySetReplacesTheJwksUrlButNotBoth) {
    env["ULW_DEV_JWKS_FILE"] = "/etc/ulw/dev-jwks.json";
    EXPECT_EQ(refused_variable(), "JWKS_URL");
    env.erase("JWKS_URL");
    const auto config = load();
    ASSERT_TRUE(config);
    EXPECT_EQ(config->dev_jwks_file, "/etc/ulw/dev-jwks.json");
    EXPECT_TRUE(config->jwks_url.empty());
}

TEST_F(ChatConfigTest, AllowedOriginsAreExactSchemeHostAndPort) {
    env["ULW_ALLOWED_ORIGINS"] = "https://app.askedin.com,http://localhost:5173";
    const auto config = load();
    ASSERT_TRUE(config);
    EXPECT_EQ(config->allowed_origins,
              (std::vector<std::string>{"https://app.askedin.com", "http://localhost:5173"}));
    for (const char* bad : {"app.askedin.com", "https://app.askedin.com/", "https://App.test",
                            "https://a.test,,https://b.test", "https://"}) {
        env["ULW_ALLOWED_ORIGINS"] = bad;
        EXPECT_EQ(refused_variable(), "ULW_ALLOWED_ORIGINS") << bad;
    }
}

} // namespace
