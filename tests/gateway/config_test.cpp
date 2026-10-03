#include "os/system_clock.hpp"

#include "config.hpp"
#include "support/memory_log.hpp"
#include "support/temp_dir.hpp"
#include "support/tls_pki.hpp"

#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

using gateway::Config;
using gateway::StorageBackend;

class ConfigTest : public ::testing::Test {
protected:
    [[nodiscard]] std::expected<Config, gateway::ConfigError> load() const {
        return gateway::load_config([this](std::string_view name) -> std::optional<std::string> {
            const auto it = env.find(std::string(name));
            return it == env.end() ? std::nullopt : std::optional<std::string>(it->second);
        });
    }

    [[nodiscard]] std::string refused_variable() const {
        const auto config = load();
        EXPECT_FALSE(config.has_value());
        return config ? std::string() : config.error().variable;
    }

    // What log_effective writes for `config`, with the settings layered from `env`.
    [[nodiscard]] std::string effective_log(const Config& config) const {
        const auto layers = ops::Settings::layer(
            gateway::settings(), nullptr,
            [this](std::string_view name) -> std::optional<std::string> {
                const auto it = env.find(std::string(name));
                return it == env.end() ? std::nullopt : std::optional<std::string>(it->second);
            },
            ops::CommandLine{});
        EXPECT_TRUE(layers);
        if (!layers) {
            return {};
        }
        const os::SystemClock clock;
        ulw::test::MemoryLog lines;
        ops::Logger log(lines, clock, "gateway", ops::Level::Info);
        gateway::log_effective(config, *layers, log);
        return lines.all();
    }

    std::map<std::string, std::string, std::less<>> env{
        {"ULW_R2_ACCOUNT_ID", "0123456789abcdef0123456789abcdef"},
        {"ULW_BUCKET", "ulw-media"},
        {"ULW_DATABASE_URL", "postgresql://ulw@db/ulw"},
        {"JWKS_URL", "https://auth.example.test/.well-known/jwks.json"},
        {"JWT_ISSUER", "https://auth.example.test"},
        {"ULW_S3_ACCESS_KEY_ID", "AKIAEXAMPLE"},
        {"ULW_S3_SECRET_ACCESS_KEY", "example-secret"},
    };
};

// RFC 8037's example Ed25519 public key.
constexpr std::string_view kKeySet = R"({"keys":[{"kty":"OKP","crv":"Ed25519","kid":"dev",)"
                                     R"("x":"11qYAYKxCrfVS_7TyWQHOg7hcvPapiMlrwIaaPcHURo"}]})";

// A key set file on disk, as ulw_devtoken jwks writes it.
class KeySetFile {
public:
    explicit KeySetFile(std::string_view text) { std::ofstream(path_) << text; }
    [[nodiscard]] std::string path() const { return path_.string(); }

private:
    ulw::test::TempDir dir_{"ulw-jwks"};
    std::filesystem::path path_ = dir_.path() / "jwks.json";
};

TEST_F(ConfigTest, TheMinimalProductionEnvironmentLoadsWithDefaults) {
    const auto config = load();
    ASSERT_TRUE(config) << config.error().variable << ": " << config.error().reason;
    EXPECT_EQ(config->port, 8080);
    EXPECT_EQ(config->reactor, net::ReactorKind::IoUring);
    EXPECT_EQ(config->storage, StorageBackend::R2);
    EXPECT_EQ(config->storage_location, "0123456789abcdef0123456789abcdef");
    EXPECT_EQ(config->bucket, "ulw-media");
    EXPECT_EQ(config->jwt_audience, "askedin-platform");
    EXPECT_EQ(config->limits.auth_cookie, "auth_token");
    EXPECT_TRUE(config->limits.allowed_origins.empty());
    EXPECT_FALSE(config->limits.allow_same_site);
    EXPECT_TRUE(config->dev_jwks_file.empty());
    EXPECT_EQ(config->transport, gateway::Transport::Plain);
}

TEST_F(ConfigTest, EachRequiredVariableIsNamedWhenMissing) {
    for (const std::string name :
         {"ULW_R2_ACCOUNT_ID", "ULW_BUCKET", "ULW_DATABASE_URL", "JWKS_URL", "JWT_ISSUER"}) {
        const std::string saved = env.at(name);
        env.erase(name);
        EXPECT_EQ(refused_variable(), name);
        env[name] = saved;
    }
}

TEST_F(ConfigTest, AnEmptyVariableCountsAsUnset) {
    env["ULW_DATABASE_URL"] = "";
    EXPECT_EQ(refused_variable(), "ULW_DATABASE_URL");
}

TEST_F(ConfigTest, KeysFetchedOverPlainHttpAreRefused) {
    env["JWKS_URL"] = "http://auth.example.test/.well-known/jwks.json";
    EXPECT_EQ(refused_variable(), "JWKS_URL");
}

TEST_F(ConfigTest, ALocalKeySetReplacesTheJwksUrl) {
    const KeySetFile file(kKeySet);
    env.erase("JWKS_URL");
    env["ULW_DEV_JWKS_FILE"] = file.path();
    env["ULW_DEV_MODE"] = "1";
    const auto config = load();
    ASSERT_TRUE(config) << config.error().reason;
    EXPECT_EQ(config->dev_jwks_file, file.path());
    EXPECT_EQ(config->dev_jwks, kKeySet);
    EXPECT_TRUE(config->jwks_url.empty());
}

// Whoever can set it signs any identity they like, so it takes development mode said outright,
// and never in a Kubernetes pod, where every real deployment runs.
TEST_F(ConfigTest, ALocalKeySetIsRefusedOutsideDevelopmentModeAndInAnyPod) {
    const KeySetFile file(kKeySet);
    env.erase("JWKS_URL");
    env["ULW_DEV_JWKS_FILE"] = file.path();
    EXPECT_EQ(refused_variable(), "ULW_DEV_JWKS_FILE");
    env["ULW_DEV_MODE"] = "0";
    EXPECT_EQ(refused_variable(), "ULW_DEV_JWKS_FILE");
    env["ULW_DEV_MODE"] = "1";
    env["KUBERNETES_SERVICE_HOST"] = "10.43.0.1";
    EXPECT_EQ(refused_variable(), "ULW_DEV_JWKS_FILE");
    env.erase("KUBERNETES_SERVICE_HOST");
    EXPECT_TRUE(load());
}

TEST_F(ConfigTest, ALocalKeySetThatCannotBeReadOrUsedIsRefused) {
    env.erase("JWKS_URL");
    env["ULW_DEV_MODE"] = "1";
    env["ULW_DEV_JWKS_FILE"] = "/nonexistent/jwks.json";
    EXPECT_EQ(refused_variable(), "ULW_DEV_JWKS_FILE");
    const KeySetFile garbage(R"({"keys":[{"kty":"RSA"}]})");
    env["ULW_DEV_JWKS_FILE"] = garbage.path();
    EXPECT_EQ(refused_variable(), "ULW_DEV_JWKS_FILE");
}

TEST_F(ConfigTest, AConnectionStringLibpqCannotReadIsRefusedWithoutQuotingIt) {
    env["ULW_DATABASE_URL"] = "postgresql://ulw:Sup3r%Secret@db/ulw";
    const auto config = load();
    ASSERT_FALSE(config);
    EXPECT_EQ(config.error().variable, "ULW_DATABASE_URL");
    EXPECT_EQ(config.error().reason.find("Sup3r"), std::string::npos);
}

TEST_F(ConfigTest, AnObjectStoreLocationOrKeysThatCannotWorkAreRefused) {
    env["ULW_R2_ACCOUNT_ID"] = "not an account";
    EXPECT_EQ(refused_variable(), "ULW_R2_ACCOUNT_ID");
    env["ULW_STORAGE"] = "minio";
    env["ULW_S3_ENDPOINT"] = "ftp://127.0.0.1:9000";
    EXPECT_EQ(refused_variable(), "ULW_S3_ENDPOINT");
    env["ULW_S3_ENDPOINT"] = "http://127.0.0.1:9000";
    env.erase("ULW_S3_SECRET_ACCESS_KEY");
    EXPECT_EQ(refused_variable(), "ULW_S3_SECRET_ACCESS_KEY");
    // The filesystem needs neither.
    env["ULW_STORAGE"] = "fs";
    env["ULW_FS_ROOT"] = "/var/lib/ulw";
    env.erase("ULW_BUCKET");
    EXPECT_TRUE(load());
}

TEST_F(ConfigTest, AnAccessKeyIdTheSignerWouldRefuseIsRefusedAtTheCheck) {
    // Each would pass a check for presence and fail the start, which restarts in a loop.
    for (const std::string& id :
         {std::string("AKIA EXAMPLE"), std::string("AKIA/EXAMPLE"), std::string(129, 'A')}) {
        env["ULW_S3_ACCESS_KEY_ID"] = id;
        EXPECT_EQ(refused_variable(), "ULW_S3_ACCESS_KEY_ID") << id;
    }
    env["ULW_S3_ACCESS_KEY_ID"] = std::string(128, 'A');
    EXPECT_TRUE(load());
}

TEST_F(ConfigTest, KeysStayTrustedADayWithoutARefreshUnlessSetInHours) {
    EXPECT_EQ(load()->jwks_max_stale_hours, 24U);
    env["ULW_JWKS_MAX_STALE_HOURS"] = "6";
    EXPECT_EQ(load()->jwks_max_stale_hours, 6U);
    for (const char* bad : {"0", "169", "1.5", "24h"}) {
        env["ULW_JWKS_MAX_STALE_HOURS"] = bad;
        EXPECT_EQ(refused_variable(), "ULW_JWKS_MAX_STALE_HOURS") << bad;
    }
}

TEST_F(ConfigTest, BothKeySourcesAtOnceAreRefused) {
    const KeySetFile file(kKeySet);
    env["ULW_DEV_JWKS_FILE"] = file.path();
    EXPECT_EQ(refused_variable(), "JWKS_URL");
}

TEST_F(ConfigTest, FilesystemStorageNeedsARootAndNoBucket) {
    env["ULW_STORAGE"] = "fs";
    env.erase("ULW_BUCKET");
    EXPECT_EQ(refused_variable(), "ULW_FS_ROOT");
    env["ULW_FS_ROOT"] = "/var/lib/ulw";
    const auto config = load();
    ASSERT_TRUE(config);
    EXPECT_EQ(config->storage, StorageBackend::Filesystem);
    EXPECT_EQ(config->storage_location, "/var/lib/ulw");
}

TEST_F(ConfigTest, MinioIsReachedThroughItsEndpoint) {
    env["ULW_STORAGE"] = "minio";
    EXPECT_EQ(refused_variable(), "ULW_S3_ENDPOINT");
    env["ULW_S3_ENDPOINT"] = "http://127.0.0.1:9000";
    const auto config = load();
    ASSERT_TRUE(config);
    EXPECT_EQ(config->storage, StorageBackend::Minio);
    EXPECT_EQ(config->storage_location, "http://127.0.0.1:9000");
}

TEST_F(ConfigTest, TlsNeedsBothTheCertificateAndTheKey) {
    env["ULW_TRANSPORT"] = "tls";
    EXPECT_EQ(refused_variable(), "ULW_TLS_CERT_FILE");
    const net::TlsFiles& files = ulw::test::TestPki::shared().server();
    env["ULW_TLS_CERT_FILE"] = files.certificate_chain;
    EXPECT_EQ(refused_variable(), "ULW_TLS_KEY_FILE");
    env["ULW_TLS_KEY_FILE"] = files.private_key;
    const auto config = load();
    ASSERT_TRUE(config) << config.error().variable << ": " << config.error().reason;
    EXPECT_EQ(config->transport, gateway::Transport::Tls);
    EXPECT_EQ(config->tls_certificate_chain, files.certificate_chain);
    EXPECT_EQ(config->tls_private_key, files.private_key);
}

TEST_F(ConfigTest, TlsFilesThatDoNotLoadAreRefusedAtTheCheckNotAtTheFirstClient) {
    const net::TlsFiles& files = ulw::test::TestPki::shared().server();
    env["ULW_TRANSPORT"] = "tls";
    env["ULW_TLS_CERT_FILE"] = files.certificate_chain;
    env["ULW_TLS_KEY_FILE"] = "/nonexistent/key.pem";
    EXPECT_EQ(refused_variable(), "ULW_TLS_CERT_FILE");
    // A certificate where the key belongs.
    env["ULW_TLS_KEY_FILE"] = files.certificate_chain;
    EXPECT_EQ(refused_variable(), "ULW_TLS_CERT_FILE");
}

TEST_F(ConfigTest, CertificateFilesWithoutTlsAreRefusedRatherThanIgnored) {
    env["ULW_TLS_CERT_FILE"] = "/run/tls/chain.pem";
    EXPECT_EQ(refused_variable(), "ULW_TLS_CERT_FILE");
    env.erase("ULW_TLS_CERT_FILE");
    env["ULW_TRANSPORT"] = "plain";
    env["ULW_TLS_KEY_FILE"] = "/run/tls/key.pem";
    EXPECT_EQ(refused_variable(), "ULW_TLS_KEY_FILE");
}

TEST_F(ConfigTest, UnknownChoicesAndOutOfRangeNumbersAreRefused) {
    const std::vector<std::pair<std::string, std::string>> bad = {
        {"ULW_STORAGE", "gcs"},        {"ULW_REACTOR", "kqueue"},     {"ULW_LISTEN_PORT", "0"},
        {"ULW_LISTEN_PORT", "65536"},  {"ULW_LISTEN_PORT", "80x"},    {"ULW_OFFLOAD_THREADS", "0"},
        {"ULW_OFFLOAD_THREADS", "65"}, {"ULW_OFFLOAD_THREADS", "-1"}, {"ULW_TRANSPORT", "ssl"},
    };
    for (const auto& [name, value] : bad) {
        env[name] = value;
        EXPECT_EQ(refused_variable(), name) << name << "=" << value;
        env.erase(name);
    }
}

TEST_F(ConfigTest, OverridesAreTakenAsGiven) {
    env["ULW_LISTEN_PORT"] = "9443";
    env["ULW_REACTOR"] = "epoll";
    env["ULW_OFFLOAD_THREADS"] = "8";
    env["JWT_AUDIENCE"] = "ulw";
    env["ULW_AUTH_COOKIE"] = "auth_token_stage";
    const auto config = load();
    ASSERT_TRUE(config);
    EXPECT_EQ(config->port, 9443);
    EXPECT_EQ(config->reactor, net::ReactorKind::Epoll);
    EXPECT_EQ(config->offload_threads, 8U);
    EXPECT_EQ(config->jwt_audience, "ulw");
    EXPECT_EQ(config->limits.auth_cookie, "auth_token_stage");
}

// The same list, parsed the same way, as chat's (http::parse_origin_list).
TEST_F(ConfigTest, AllowedOriginsAreExactSchemeHostAndPort) {
    env["ULW_ALLOWED_ORIGINS"] = "https://app.askedin.com,http://localhost:5173";
    env["ULW_ALLOW_SAME_SITE"] = "1";
    const auto config = load();
    ASSERT_TRUE(config);
    EXPECT_EQ(config->limits.allowed_origins,
              (std::vector<std::string>{"https://app.askedin.com", "http://localhost:5173"}));
    EXPECT_TRUE(config->limits.allow_same_site);
    for (const char* bad : {"app.askedin.com", "https://app.askedin.com/", "https://App.test",
                            "https://a.test,,https://b.test", "https://", "*"}) {
        env["ULW_ALLOWED_ORIGINS"] = bad;
        EXPECT_EQ(refused_variable(), "ULW_ALLOWED_ORIGINS") << bad;
    }
    env.erase("ULW_ALLOWED_ORIGINS");
    for (const char* bad : {"yes", "true", "2"}) {
        env["ULW_ALLOW_SAME_SITE"] = bad;
        EXPECT_EQ(refused_variable(), "ULW_ALLOW_SAME_SITE") << bad;
    }
}

// Plain http would carry the cookie in the clear, so it names only a dev server on this machine.
// A browser leaves the scheme's own port out of Origin, so an entry naming it could never match.
TEST_F(ConfigTest, PlainHttpOriginsAreLoopbackOnlyAndDefaultPortsAreRefused) {
    env["ULW_ALLOWED_ORIGINS"] = "http://localhost:5173,http://127.0.0.1:8080,http://[::1]:3000";
    const auto config = load();
    ASSERT_TRUE(config);
    EXPECT_EQ(config->limits.allowed_origins,
              (std::vector<std::string>{"http://localhost:5173", "http://127.0.0.1:8080",
                                        "http://[::1]:3000"}));
    for (const char* bad : {"http://app.example", "http://app.example:8080", "http://10.0.0.1",
                            "https://app.example:443", "http://localhost:80", "https://:443",
                            "https://app.example:", "http://[::1"}) {
        env["ULW_ALLOWED_ORIGINS"] = bad;
        EXPECT_EQ(refused_variable(), "ULW_ALLOWED_ORIGINS") << bad;
    }
}

// Only a port, host and bytes a browser could write in Origin: the default port is caught by
// value, no prefix or userinfo dresses another host up as loopback, and an IPv4 host is four
// decimal octets.
TEST_F(ConfigTest, OriginsNoBrowserSendsAreRefused) {
    env["ULW_ALLOWED_ORIGINS"] = "https://a.example:1,https://a.example:65535,https://[::1]";
    const auto config = load();
    ASSERT_TRUE(config);
    EXPECT_EQ(config->limits.allowed_origins.size(), 3U);
    for (const char* bad : {"https://a.example:abc",
                            "http://localhost:abc",
                            "https://a:b:c",
                            "http://localhost:3000:4000",
                            "http://[::1]:x",
                            "https://a.example:0",
                            "https://a.example:65536",
                            "https://a.example:123456",
                            "http://localhost:0080",
                            "https://a.example:0443",
                            "https://a.example\x01",
                            "https://caf\xc3\xa9.example",
                            "https://a.example\t",
                            "https://[]",
                            "https://[]:443",
                            "http://localhost.evil.example",
                            "http://localhost@evil.example",
                            "http://127.0.0.1.evil.example",
                            "https://010.0.0.1",
                            "https://0x7f.1",
                            "https://127.1",
                            "https://256.0.0.1",
                            "https://10.0.0.1.",
                            "https://1.",
                            "https://127.",
                            "https://0x7f."}) {
        env["ULW_ALLOWED_ORIGINS"] = bad;
        EXPECT_EQ(refused_variable(), "ULW_ALLOWED_ORIGINS") << bad;
    }
}

// The scheme is matched exactly, the authority follows a single "://", and an IPv6 literal is
// written as a browser writes it (RFC 5952 form, no dotted IPv4 tail): another spelling of the
// same address could never match, and is refused rather than listed.
TEST_F(ConfigTest, OriginsWithAMalformedSchemeOrIpv6LiteralAreRefused) {
    env["ULW_ALLOWED_ORIGINS"] = "https://[2001:db8::1],https://[::ffff:c000:201]:8443";
    const auto config = load();
    ASSERT_TRUE(config);
    EXPECT_EQ(config->limits.allowed_origins.size(), 2U);
    for (const char* bad :
         {"HTTPS://a.example", "://a.example", "https:/a.example", "https:///a",
          "https://https://a", "https://[zz]", "https://[zz]:8443", "https://[::1%eth0]",
          "https://[a.example]", "https://[::ffff:192.0.2.1]:8443", "https://[0:0:0:0:0:0:0:1]",
          "https://[2001:db8:0::1]", "https://[::01]", "https://[1::2::3]"}) {
        env["ULW_ALLOWED_ORIGINS"] = bad;
        EXPECT_EQ(refused_variable(), "ULW_ALLOWED_ORIGINS") << bad;
    }
}

TEST_F(ConfigTest, TheFilesystemBackendTakesAFileServerForSegmentUrls) {
    env["ULW_STORAGE"] = "fs";
    env["ULW_FS_ROOT"] = "/var/lib/ulw";
    const auto without = load();
    ASSERT_TRUE(without);
    EXPECT_TRUE(without->limits.local_read_url.empty());

    env["ULW_FS_READ_URL"] = "http://127.0.0.1:8081/objects//";
    const auto with = load();
    ASSERT_TRUE(with);
    EXPECT_EQ(with->limits.local_read_url, "http://127.0.0.1:8081/objects");

    env["ULW_FS_READ_URL"] = "file:///var/lib/ulw/objects";
    EXPECT_EQ(refused_variable(), "ULW_FS_READ_URL");
}

TEST_F(ConfigTest, AFileServerBesideAnObjectStoreIsRefused) {
    env["ULW_FS_READ_URL"] = "http://127.0.0.1:8081";
    EXPECT_EQ(refused_variable(), "ULW_FS_READ_URL");
}

TEST_F(ConfigTest, AdmissionLimitsDefaultToTheDerivedBudgetAndMayBeLowered) {
    const auto defaults = load();
    ASSERT_TRUE(defaults);
    EXPECT_EQ(defaults->limits.max_connections, 448U);
    EXPECT_EQ(defaults->limits.max_upload_slots, 448U);
    EXPECT_EQ(defaults->limits.max_uploads_per_user, 3U);
    EXPECT_EQ(defaults->chunk_size, 8U << 20U);
    EXPECT_EQ(defaults->log_level, ops::Level::Info);

    // Lowering connections alone takes the slots down with it rather than refusing.
    env["ULW_MAX_CONNECTIONS"] = "100";
    const auto lowered = load();
    ASSERT_TRUE(lowered);
    EXPECT_EQ(lowered->limits.max_upload_slots, 100U);
    env["ULW_MAX_UPLOAD_SLOTS"] = "2";
    const auto few = load();
    ASSERT_TRUE(few);
    EXPECT_EQ(few->limits.max_uploads_per_user, 2U);
}

TEST_F(ConfigTest, LimitsThatContradictEachOtherAreRefused) {
    env["ULW_MAX_CONNECTIONS"] = "100";
    env["ULW_MAX_UPLOAD_SLOTS"] = "101";
    EXPECT_EQ(refused_variable(), "ULW_MAX_UPLOAD_SLOTS");
    env["ULW_MAX_UPLOAD_SLOTS"] = "10";
    env["ULW_MAX_UPLOADS_PER_USER"] = "11";
    EXPECT_EQ(refused_variable(), "ULW_MAX_UPLOADS_PER_USER");
    env["ULW_MAX_UPLOADS_PER_USER"] = "0";
    EXPECT_EQ(refused_variable(), "ULW_MAX_UPLOADS_PER_USER");
}

TEST_F(ConfigTest, PerClientLimitsDefaultToTheBriefsAndTrustNoProxy) {
    const auto defaults = load();
    ASSERT_TRUE(defaults);
    EXPECT_EQ(defaults->limits.max_connections_per_ip, 20U);
    EXPECT_EQ(defaults->limits.new_connections_per_ip_per_second, 10U);
    EXPECT_EQ(defaults->limits.requests_per_user_per_minute, 300U);
    EXPECT_EQ(defaults->limits.upload_bytes_per_user_per_day, std::uint64_t{100} << 30U);
    EXPECT_TRUE(defaults->limits.trusted_proxies.empty());
    EXPECT_TRUE(defaults->run_as_user.empty());
    EXPECT_FALSE(defaults->allow_root);
    EXPECT_EQ(defaults->limits.trusted_proxy_hops, 1U);

    env["ULW_MAX_CONNECTIONS_PER_IP"] = "64";
    env["ULW_NEW_CONNECTIONS_PER_IP_PER_SECOND"] = "50";
    env["ULW_REQUESTS_PER_USER_PER_MINUTE"] = "600";
    env["ULW_UPLOAD_BYTES_PER_USER_PER_DAY"] = std::to_string(std::uint64_t{16} << 20U);
    env["ULW_TRUSTED_PROXIES"] = "10.42.0.0/16,\tfd00:42::/56";
    env["ULW_RUN_AS_USER"] = "ulw";
    const auto set = load();
    ASSERT_TRUE(set);
    EXPECT_EQ(set->limits.max_connections_per_ip, 64U);
    EXPECT_EQ(set->limits.new_connections_per_ip_per_second, 50U);
    EXPECT_EQ(set->limits.requests_per_user_per_minute, 600U);
    EXPECT_EQ(set->limits.upload_bytes_per_user_per_day, std::uint64_t{16} << 20U);
    EXPECT_EQ(set->run_as_user, "ulw");
    EXPECT_EQ(set->limits.trusted_proxy_hops, 1U);
    ASSERT_EQ(set->limits.trusted_proxies.size(), 2U);
    const auto pod = net::IpAddress::parse("10.42.7.1");
    const auto outside = net::IpAddress::parse("10.43.0.1");
    ASSERT_TRUE(pod && outside);
    EXPECT_TRUE(set->limits.trusted_proxies[0].contains(*pod));
    EXPECT_FALSE(set->limits.trusted_proxies[0].contains(*outside));
}

TEST_F(ConfigTest, PerClientLimitsThatCouldNeverBeMetAreRefused) {
    env["ULW_MAX_CONNECTIONS"] = "100";
    env["ULW_MAX_CONNECTIONS_PER_IP"] = "101";
    EXPECT_EQ(refused_variable(), "ULW_MAX_CONNECTIONS_PER_IP");
    env.erase("ULW_MAX_CONNECTIONS_PER_IP");
    env["ULW_NEW_CONNECTIONS_PER_IP_PER_SECOND"] = "0";
    EXPECT_EQ(refused_variable(), "ULW_NEW_CONNECTIONS_PER_IP_PER_SECOND");
    env.erase("ULW_NEW_CONNECTIONS_PER_IP_PER_SECOND");
    env["ULW_REQUESTS_PER_USER_PER_MINUTE"] = "0";
    EXPECT_EQ(refused_variable(), "ULW_REQUESTS_PER_USER_PER_MINUTE");
    env.erase("ULW_REQUESTS_PER_USER_PER_MINUTE");
    // One byte short of the largest PATCH body, which could then never be admitted.
    env["ULW_UPLOAD_BYTES_PER_USER_PER_DAY"] = std::to_string((std::uint64_t{16} << 20U) - 1);
    EXPECT_EQ(refused_variable(), "ULW_UPLOAD_BYTES_PER_USER_PER_DAY");
}

TEST_F(ConfigTest, TrustedProxiesMustBeExactBlocks) {
    for (const std::string_view bad :
         {"10.42.0.0/33", "10.42.1.0/16", "envoy", "10.42.0.0/16;10.43.0.0/16", "10.42.0.0/16,,",
          "0.0.0.0/0", "10.42.0.0/16, ::/0"}) {
        env["ULW_TRUSTED_PROXIES"] = std::string(bad);
        EXPECT_EQ(refused_variable(), "ULW_TRUSTED_PROXIES") << bad;
    }
    std::string many;
    for (int i = 0; i < 17; ++i) {
        many += (many.empty() ? "" : ",") + std::format("10.{}.0.0/16", i);
    }
    env["ULW_TRUSTED_PROXIES"] = many;
    EXPECT_EQ(refused_variable(), "ULW_TRUSTED_PROXIES");
}

TEST_F(ConfigTest, ProxyHopsCountOnlyWithTrustedProxies) {
    env["ULW_TRUSTED_PROXY_HOPS"] = "2";
    EXPECT_EQ(refused_variable(), "ULW_TRUSTED_PROXY_HOPS");
    env["ULW_TRUSTED_PROXIES"] = "10.42.0.0/16";
    const auto two = load();
    ASSERT_TRUE(two);
    EXPECT_EQ(two->limits.trusted_proxy_hops, 2U);
    for (const std::string_view bad : {"0", "17", "one"}) {
        env["ULW_TRUSTED_PROXY_HOPS"] = std::string(bad);
        EXPECT_EQ(refused_variable(), "ULW_TRUSTED_PROXY_HOPS") << bad;
    }
}

TEST_F(ConfigTest, StayingRootIsAnExplicitChoice) {
    env["ULW_ALLOW_ROOT"] = "1";
    const auto allowed = load();
    ASSERT_TRUE(allowed);
    EXPECT_TRUE(allowed->allow_root);
    env["ULW_ALLOW_ROOT"] = "yes";
    EXPECT_EQ(refused_variable(), "ULW_ALLOW_ROOT");
}

TEST_F(ConfigTest, AChunkTheObjectStoreWouldRefuseIsRefusedAtStartup) {
    // One byte under 5 MiB, and the largest the 10,000-part cap does not reach.
    env["ULW_CHUNK_SIZE"] = std::to_string((5U << 20U) - 1);
    EXPECT_EQ(refused_variable(), "ULW_CHUNK_SIZE");
    env["ULW_CHUNK_SIZE"] = std::to_string(5U << 20U);
    EXPECT_EQ(refused_variable(), "ULW_CHUNK_SIZE");
    env["ULW_CHUNK_SIZE"] = "5368710";
    const auto smallest = load();
    ASSERT_TRUE(smallest);
    EXPECT_EQ(smallest->chunk_size, 5'368'710U);
    env["ULW_CHUNK_SIZE"] = std::to_string((std::uint64_t{5} << 30U) + 1);
    EXPECT_EQ(refused_variable(), "ULW_CHUNK_SIZE");
}

TEST_F(ConfigTest, TheLogLevelIsOneOfFour) {
    env["ULW_LOG_LEVEL"] = "debug";
    ASSERT_TRUE(load());
    EXPECT_EQ(load()->log_level, ops::Level::Debug);
    env["ULW_LOG_LEVEL"] = "trace";
    EXPECT_EQ(refused_variable(), "ULW_LOG_LEVEL");
}

TEST(DescriptorBudget, TwoDescriptorsPerConnectionAfterTheReserve) {
    gateway::Limits limits;
    limits.max_connections = 448;
    // (960 - 64) / 2 = 448 exactly; one descriptor fewer leaves room for 447.
    EXPECT_TRUE(gateway::check_descriptor_budget(limits, 960));
    const auto short_by_one = gateway::check_descriptor_budget(limits, 959);
    ASSERT_FALSE(short_by_one);
    EXPECT_EQ(short_by_one.error().variable, "ULW_MAX_CONNECTIONS");
    EXPECT_FALSE(gateway::check_descriptor_budget(limits, 64));
    EXPECT_FALSE(gateway::check_descriptor_budget(limits, 0));
}

TEST_F(ConfigTest, OnlyTheConnectionStringTheStoreKeysAndLiveKitsSecretAreSecret) {
    for (const ops::Setting& s : gateway::settings()) {
        const bool store_key =
            s.env == "ULW_S3_ACCESS_KEY_ID" || s.env == "ULW_S3_SECRET_ACCESS_KEY";
        // The kubelet's, read to know the process runs in a pod, and PATH, passed on to a
        // packager the gateway starts: nobody configures either.
        EXPECT_EQ(s.key.empty(),
                  store_key || s.env == "KUBERNETES_SERVICE_HOST" || s.env == "PATH")
            << s.env;
        EXPECT_EQ(s.secret,
                  store_key || s.env == "ULW_DATABASE_URL" || s.env == "LIVEKIT_API_SECRET")
            << s.env;
    }
}

// The stream service (ADR-0091): off unless LiveKit is named, and then complete.
class LiveConfigTest : public ConfigTest {
protected:
    // Every live setting as a working deployment has them, whatever an earlier step set.
    void live(std::string_view runtime) {
        std::erase_if(env, [](const auto& e) {
            return e.first.starts_with("LIVEKIT_") || e.first.starts_with("ULW_LIVE_") ||
                   e.first.starts_with("ULW_K8S_");
        });
        env["LIVEKIT_API_URL"] = "http://livekit:7880";
        env["LIVEKIT_CLIENT_URL"] = "wss://media.example.test";
        env["LIVEKIT_API_KEY"] = "fake-key";
        env["LIVEKIT_API_SECRET"] = "fake-secret-testtest123-testtest123";
        env["ULW_LIVE_PACKAGER_SRT"] = "srt://{stream}.live-packager.apps.svc.cluster.local:9000";
        env["ULW_LIVE_PACKAGER"] = std::string(runtime);
        if (runtime == "process") {
            env["ULW_LIVE_PACKAGER_BIN"] = "/opt/ulw/live_packager";
            env["ULW_LIVE_PACKAGER_SCRATCH_DIR"] = "/var/cache/ulw-live";
            env["ULW_LIVE_MAX_STREAMS"] = "1";
        } else {
            std::ofstream(template_file()) << "metadata:\n  name: ${ULW_STREAM_ID}\n";
            env["ULW_LIVE_JOB_TEMPLATE"] = template_file();
            env["ULW_LIVE_PACKAGER_IMAGE_TAG"] = "main";
            env["ULW_K8S_NAMESPACE"] = "apps-test";
        }
    }
    [[nodiscard]] std::string template_file() const { return (dir.path() / "job.yaml").string(); }

    ulw::test::TempDir dir{"ulw-live-config"};
};

TEST_F(LiveConfigTest, LiveStreamsAreOffUnlessLiveKitIsNamed) {
    const auto config = load();
    ASSERT_TRUE(config);
    EXPECT_FALSE(config->live.enabled);
    for (const char* stray : {"LIVEKIT_CLIENT_URL", "ULW_LIVE_PACKAGER", "ULW_LIVE_PACKAGER_SRT"}) {
        auto with = env;
        env[stray] = "x";
        EXPECT_EQ(refused_variable(), stray);
        env = with;
    }
}

TEST_F(LiveConfigTest, TheProcessRuntimeHandsAPackagerTheGatewaysStoreAndDatabaseOnly) {
    live("process");
    env["PATH"] = "/usr/bin:/bin";
    const auto config = load();
    ASSERT_TRUE(config) << config.error().variable << ": " << config.error().reason;
    const gateway::LiveConfig& l = config->live;
    EXPECT_TRUE(l.enabled);
    EXPECT_EQ(l.runtime, gateway::PackagerRuntime::Process);
    EXPECT_EQ(l.packager_binary, "/opt/ulw/live_packager");
    EXPECT_EQ(l.settings.max_streams, 1U);
    EXPECT_EQ(l.settings.segment, core::Seconds{2});
    const std::vector<std::string> expected{
        "ULW_STORAGE=r2",
        "ULW_R2_ACCOUNT_ID=0123456789abcdef0123456789abcdef",
        "ULW_BUCKET=ulw-media",
        "ULW_S3_ACCESS_KEY_ID=AKIAEXAMPLE",
        "ULW_S3_SECRET_ACCESS_KEY=example-secret",
        "ULW_DATABASE_URL=postgresql://ulw@db/ulw",
        "ULW_SCRATCH_DIR=/var/cache/ulw-live",
        "ULW_LIVE_INGEST_PORT=9000",
        "ULW_LIVE_SEGMENT_SECONDS=2",
        "ULW_LIVE_CALLER_WAIT_SECONDS=60",
        "PATH=/usr/bin:/bin",
    };
    EXPECT_EQ(l.packager_environment, expected);
    // LiveKit's secret is logged redacted.
    const std::string all = effective_log(*config);
    EXPECT_EQ(all.find("fake-secret-testtest123"), std::string::npos) << all;
    EXPECT_NE(all.find(R"("name":"ULW_LIVE_PACKAGER","value":"process")"), std::string::npos);
}

TEST_F(LiveConfigTest, TheProcessRuntimeFollowsTheStoreItIsGiven) {
    live("process");
    env.erase("ULW_R2_ACCOUNT_ID");
    env["ULW_STORAGE"] = "minio";
    env["ULW_S3_ENDPOINT"] = "http://127.0.0.1:9000";
    const auto minio = load();
    ASSERT_TRUE(minio);
    EXPECT_EQ(minio->live.packager_environment.at(1), "ULW_S3_ENDPOINT=http://127.0.0.1:9000");
    env["ULW_STORAGE"] = "fs";
    env["ULW_FS_ROOT"] = "/srv/ulw";
    const auto fs = load();
    ASSERT_TRUE(fs);
    EXPECT_EQ(fs->live.packager_environment.at(1), "ULW_FS_ROOT=/srv/ulw");
    EXPECT_EQ(fs->live.packager_environment.at(2), "ULW_DATABASE_URL=postgresql://ulw@db/ulw");
}

TEST_F(LiveConfigTest, TheProcessRuntimeRunsOneStreamAtATime) {
    live("process");
    env["ULW_LIVE_MAX_STREAMS"] = "2";
    EXPECT_EQ(refused_variable(), "ULW_LIVE_MAX_STREAMS");
    env["ULW_LIVE_MAX_STREAMS"] = "1";
    env["ULW_LIVE_PACKAGER_BIN"] = "live_packager";
    EXPECT_EQ(refused_variable(), "ULW_LIVE_PACKAGER_BIN");
    env["ULW_LIVE_PACKAGER_BIN"] = "/opt/ulw/live_packager";
    env["ULW_LIVE_PACKAGER_SCRATCH_DIR"] = "scratch";
    EXPECT_EQ(refused_variable(), "ULW_LIVE_PACKAGER_SCRATCH_DIR");
    env.erase("ULW_LIVE_PACKAGER_SCRATCH_DIR");
    EXPECT_EQ(refused_variable(), "ULW_LIVE_PACKAGER_SCRATCH_DIR");
}

TEST_F(LiveConfigTest, TheKubernetesRuntimeReadsItsTemplate) {
    live("kubernetes");
    env["ULW_LIVE_MAX_STREAMS"] = "2";
    env["ULW_K8S_API_URL"] = "https://10.43.0.1";
    env["ULW_K8S_TOKEN_FILE"] = "/run/token";
    env["ULW_K8S_CA_FILE"] = "/run/ca.crt";
    const auto config = load();
    ASSERT_TRUE(config) << config.error().variable << ": " << config.error().reason;
    const gateway::LiveConfig& l = config->live;
    EXPECT_EQ(l.runtime, gateway::PackagerRuntime::Kubernetes);
    EXPECT_EQ(l.job_template, "metadata:\n  name: ${ULW_STREAM_ID}\n");
    EXPECT_EQ(l.image_tag, "main");
    EXPECT_EQ(l.k8s_namespace, "apps-test");
    EXPECT_EQ(l.k8s_api_url, "https://10.43.0.1");
    EXPECT_EQ(l.k8s_token_file, "/run/token");
    EXPECT_EQ(l.k8s_ca_file, "/run/ca.crt");
    EXPECT_EQ(l.settings.max_streams, 2U);
    EXPECT_NE(effective_log(*config).find(R"("name":"ULW_K8S_NAMESPACE","value":"apps-test")"),
              std::string::npos);
}

TEST_F(LiveConfigTest, TheKubernetesRuntimeRefusesWhatCouldNotWork) {
    live("kubernetes");
    std::ofstream(template_file()) << "metadata:\n  name: fixed\n";
    EXPECT_EQ(refused_variable(), "ULW_LIVE_JOB_TEMPLATE");
    env["ULW_LIVE_JOB_TEMPLATE"] = (dir.path() / "missing.yaml").string();
    EXPECT_EQ(refused_variable(), "ULW_LIVE_JOB_TEMPLATE");
    live("kubernetes");
    env.erase("ULW_LIVE_PACKAGER_IMAGE_TAG");
    EXPECT_EQ(refused_variable(), "ULW_LIVE_PACKAGER_IMAGE_TAG");
    live("kubernetes");
    env["ULW_K8S_API_URL"] = "kubernetes.default.svc";
    EXPECT_EQ(refused_variable(), "ULW_K8S_API_URL");
    live("kubernetes");
    env.erase("ULW_K8S_NAMESPACE");
    // No service account here to name one.
    EXPECT_EQ(refused_variable(), "ULW_K8S_NAMESPACE");
}

TEST_F(LiveConfigTest, EveryLiveSettingIsChecked) {
    const std::vector<std::pair<std::string, std::string>> bad{
        {"LIVEKIT_API_URL", "livekit:7880"},
        {"LIVEKIT_CLIENT_URL", "https://media.example.test"},
        {"ULW_LIVE_PACKAGER_SRT", "udp://packager:9000"},
        {"ULW_LIVE_PACKAGER", "docker"},
        {"ULW_LIVE_SEGMENT_SECONDS", "1"},
        {"ULW_LIVE_MAX_STREAMS", "0"},
        {"ULW_LIVE_START_WINDOW_SECONDS", "59"},
        {"ULW_LIVE_PACKAGER_PORT", "0"},
    };
    for (const auto& [name, value] : bad) {
        live("process");
        env[name] = value;
        EXPECT_EQ(refused_variable(), name) << value;
    }
    for (const char* missing : {"LIVEKIT_CLIENT_URL", "LIVEKIT_API_KEY", "LIVEKIT_API_SECRET",
                                "ULW_LIVE_PACKAGER_SRT", "ULW_LIVE_PACKAGER"}) {
        live("process");
        env.erase(missing);
        EXPECT_EQ(refused_variable(), missing);
    }
    live("process");
    env["ULW_LIVE_START_WINDOW_SECONDS"] = "120";
    env["ULW_LIVE_SEGMENT_SECONDS"] = "4";
    const auto config = load();
    ASSERT_TRUE(config);
    EXPECT_EQ(config->live.settings.start_window, core::Seconds{120});
    EXPECT_EQ(config->live.settings.segment, core::Seconds{4});
}

TEST_F(ConfigTest, TheEffectiveConfigurationIsLoggedWithTheSecretRedacted) {
    env["ULW_DATABASE_URL"] = "postgresql://ulw:hunter2@db/ulw";
    env["ULW_LISTEN_PORT"] = "9000";
    const auto config = load();
    ASSERT_TRUE(config);
    const std::string all = effective_log(*config);
    EXPECT_EQ(all.find("hunter2"), std::string::npos);
    EXPECT_NE(all.find(R"("name":"ULW_DATABASE_URL","value":"<redacted>","from":"env")"),
              std::string::npos)
        << all;
    EXPECT_NE(all.find(R"("name":"ULW_LISTEN_PORT","value":"9000","from":"env")"),
              std::string::npos);
    EXPECT_NE(all.find(R"("name":"ULW_MAX_CONNECTIONS","value":"448","from":"default")"),
              std::string::npos);
}

// A development run says so beside the key set it trusts, and where the switch came from.
TEST_F(ConfigTest, DevelopmentModeIsLoggedWithTheLocalKeySet) {
    const KeySetFile file(kKeySet);
    env.erase("JWKS_URL");
    env["ULW_DEV_JWKS_FILE"] = file.path();
    env["ULW_DEV_MODE"] = "1";
    const auto config = load();
    ASSERT_TRUE(config) << config.error().reason;
    const std::string all = effective_log(*config);
    const auto keys = all.find(R"("name":"ULW_DEV_JWKS_FILE")");
    const auto mode = all.find(R"("name":"ULW_DEV_MODE","value":"1","from":"env")");
    ASSERT_NE(keys, std::string::npos) << all;
    ASSERT_NE(mode, std::string::npos) << all;
    // The line after it.
    EXPECT_EQ(all.find('\n', keys), all.rfind('\n', mode)) << all;
}

} // namespace
