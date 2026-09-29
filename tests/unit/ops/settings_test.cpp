#include "ops/settings.hpp"
#include "support/temp_dir.hpp"

#include <sys/stat.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <string>
#include <vector>

namespace {

using ops::Origin;

constexpr std::array kSchema{
    ops::Setting{.env = "ULW_LISTEN_PORT", .key = "listen.port"},
    ops::Setting{.env = "ULW_LOG_LEVEL", .key = "log.level"},
    ops::Setting{.env = "ULW_DATABASE_URL", .key = "database.url", .secret = true},
    ops::Setting{.env = "HOSTNAME", .key = ""},
};

class SettingsTest : public ::testing::Test {
protected:
    [[nodiscard]] ops::Lookup env() const {
        return [this](std::string_view name) -> std::optional<std::string> {
            const auto it = vars.find(std::string(name));
            return it == vars.end() ? std::nullopt : std::optional(it->second);
        };
    }

    [[nodiscard]] static std::expected<ops::CommandLine, ops::SettingsError>
    cli(std::vector<std::string_view> args) {
        return ops::parse_command_line(kSchema, args);
    }

    [[nodiscard]] std::expected<ops::Settings, ops::SettingsError>
    layer(const ops::CommandLine& c, const std::string& toml = "", bool private_file = true) const {
        if (toml.empty()) {
            return ops::Settings::layer(kSchema, nullptr, env(), c);
        }
        const auto entries = ops::toml::parse(toml);
        EXPECT_TRUE(entries);
        const ops::FileLayer file{
            .path = "/etc/ulw/gateway.toml", .entries = *entries, .private_to_owner = private_file};
        return ops::Settings::layer(kSchema, &file, env(), c);
    }

    std::map<std::string, std::string> vars;
};

TEST_F(SettingsTest, EachLayerOverridesTheOneBelow) {
    const std::string file = "[listen]\nport = 1000\n[log]\nlevel = \"warn\"\n"
                             "[database]\nurl = \"postgresql://file\"\n";
    vars["ULW_LISTEN_PORT"] = "2000";
    vars["ULW_DATABASE_URL"] = "postgresql://env";
    const auto c = cli({"--listen-port=3000"});
    ASSERT_TRUE(c);
    const auto s = layer(*c, file);
    ASSERT_TRUE(s) << s.error().reason;
    EXPECT_EQ(s->get("ULW_LISTEN_PORT"), "3000");
    EXPECT_EQ(s->origin("ULW_LISTEN_PORT"), Origin::CommandLine);
    EXPECT_EQ(s->get("ULW_DATABASE_URL"), "postgresql://env");
    EXPECT_EQ(s->origin("ULW_DATABASE_URL"), Origin::Environment);
    EXPECT_EQ(s->get("ULW_LOG_LEVEL"), "warn");
    EXPECT_EQ(s->origin("ULW_LOG_LEVEL"), Origin::File);
    EXPECT_EQ(s->get("HOSTNAME"), std::nullopt);
    EXPECT_EQ(s->origin("HOSTNAME"), Origin::Default);
    EXPECT_EQ(s->lookup()("ULW_LISTEN_PORT"), "3000");
}

TEST_F(SettingsTest, AnEmptyVariableDoesNotHideTheFile) {
    vars["ULW_LISTEN_PORT"] = "";
    const auto s = layer(ops::CommandLine{}, "listen.port = 1000\n");
    ASSERT_TRUE(s);
    EXPECT_EQ(s->get("ULW_LISTEN_PORT"), "1000");
}

TEST_F(SettingsTest, VariablesOutsideTheSchemaAreNotSeen) {
    vars["ULW_SOMETHING_ELSE"] = "x";
    const auto s = layer(ops::CommandLine{});
    ASSERT_TRUE(s);
    EXPECT_EQ(s->lookup()("ULW_SOMETHING_ELSE"), std::nullopt);
}

TEST_F(SettingsTest, FlagsTakeTheirValueAttachedOrAsTheNextArgument) {
    const auto c = cli({"--listen-port", "81", "--log-level=debug", "--config", "/etc/g.toml",
                        "--check-config", "--version"});
    ASSERT_TRUE(c) << c.error().source;
    EXPECT_EQ(c->values.at("ULW_LISTEN_PORT"), "81");
    EXPECT_EQ(c->values.at("ULW_LOG_LEVEL"), "debug");
    EXPECT_EQ(c->config_file, "/etc/g.toml");
    EXPECT_TRUE(c->check);
    EXPECT_TRUE(c->version);
}

TEST_F(SettingsTest, BadCommandLinesNameTheArgumentAtFault) {
    EXPECT_EQ(cli({"--nope=1"}).error().source, "--nope");
    EXPECT_EQ(cli({"stray"}).error().reason, "unexpected argument");
    EXPECT_EQ(cli({"--listen-port"}).error().reason, "needs a value");
    EXPECT_EQ(cli({"--listen-port=1", "--listen-port=2"}).error().reason, "given twice");
    // A variable that is only ever set in the environment has no flag.
    EXPECT_EQ(cli({"--hostname=h"}).error().reason, "unknown option");
}

TEST_F(SettingsTest, SecretsAreNeverTakenFromTheCommandLine) {
    const auto c = cli({"--database-url=postgresql://u:pw@h/db"});
    ASSERT_FALSE(c);
    EXPECT_EQ(c.error().source, "--database-url");
    // The value itself is not repeated back.
    EXPECT_EQ(c.error().reason.find("pw"), std::string::npos);
}

TEST_F(SettingsTest, ASecretInAFileOthersMayReadIsRefused) {
    const auto open = layer(ops::CommandLine{}, "database.url = \"postgresql://x\"\n", false);
    ASSERT_FALSE(open);
    EXPECT_EQ(open.error().source, "/etc/ulw/gateway.toml:1: database.url");
    EXPECT_TRUE(layer(ops::CommandLine{}, "listen.port = 1\n", false));
    EXPECT_TRUE(layer(ops::CommandLine{}, "database.url = \"postgresql://x\"\n", true));
}

TEST_F(SettingsTest, AnUnknownFileKeyIsAnErrorNotIgnored) {
    const auto s = layer(ops::CommandLine{}, "[listen]\nprot = 1\n");
    ASSERT_FALSE(s);
    EXPECT_EQ(s.error().source, "/etc/ulw/gateway.toml:2: listen.prot");
    EXPECT_EQ(s.error().reason, "unknown setting");
}

class ConfigFileTest : public SettingsTest {
protected:
    std::string write(const std::string& text, mode_t mode) {
        const std::string path = (dir.path() / "gateway.toml").string();
        std::ofstream(path) << text;
        EXPECT_EQ(::chmod(path.c_str(), mode), 0);
        return path;
    }

    ulw::test::TempDir dir{"ulw-settings"};
};

TEST_F(ConfigFileTest, TheFileNamedOnTheCommandLineIsLayeredUnderTheEnvironment) {
    const std::string path = write("listen.port = 1000\nlog.level = \"warn\"\n", 0644);
    vars["ULW_LOG_LEVEL"] = "error";
    const auto c = cli({"--config", path});
    ASSERT_TRUE(c);
    const auto s = ops::load_settings(kSchema, *c, env());
    ASSERT_TRUE(s) << s.error().source << ": " << s.error().reason;
    EXPECT_EQ(s->get("ULW_LISTEN_PORT"), "1000");
    EXPECT_EQ(s->get("ULW_LOG_LEVEL"), "error");
}

TEST_F(ConfigFileTest, UlwConfigNamesTheFileWhenTheCommandLineDoesNot) {
    vars["ULW_CONFIG"] = write("listen.port = 1000\n", 0600);
    const auto s = ops::load_settings(kSchema, ops::CommandLine{}, env());
    ASSERT_TRUE(s);
    EXPECT_EQ(s->origin("ULW_LISTEN_PORT"), Origin::File);
}

TEST_F(ConfigFileTest, OnlyAnOwnerOnlyFileMayHoldASecret) {
    const std::string text = "database.url = \"postgresql://u:pw@h/db\"\n";
    for (const mode_t mode : {0644U, 0640U, 0604U}) {
        const auto s = ops::load_settings(kSchema, *cli({"--config", write(text, mode)}), env());
        EXPECT_FALSE(s) << std::oct << mode;
    }
    for (const mode_t mode : {0400U, 0600U}) {
        const auto s = ops::load_settings(kSchema, *cli({"--config", write(text, mode)}), env());
        EXPECT_TRUE(s) << std::oct << mode;
    }
}

TEST_F(ConfigFileTest, AFifoIsRefusedRatherThanWaitedOn) {
    const std::string path = (dir.path() / "gateway.toml").string();
    ASSERT_EQ(::mkfifo(path.c_str(), 0600), 0);
    const auto s = ops::load_settings(kSchema, *cli({"--config", path}), env());
    ASSERT_FALSE(s);
    EXPECT_EQ(s.error().reason, "not a regular file");
}

TEST_F(ConfigFileTest, AMalformedOrMissingFileIsNamedWithTheLine) {
    const auto bad =
        ops::load_settings(kSchema, *cli({"--config", write("a = [1]\n", 0600)}), env());
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().source, (dir.path() / "gateway.toml").string() + ":1");
    EXPECT_EQ(bad.error().reason, "arrays are not supported");
    const auto missing =
        ops::load_settings(kSchema, *cli({"--config", "/nonexistent/g.toml"}), env());
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().source, "/nonexistent/g.toml");
}

} // namespace
