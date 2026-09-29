// gateway_server as the service manager runs it: its version, its refusal of bad
// configuration with exit code 2, the layering of file, environment and flags, readiness that
// follows the database, the notify protocol, and the SIGTERM drain.
#include "core/version.hpp"
#include "os/system_clock.hpp"

#include "devtoken/dev_key.hpp"
#include "postgres_harness.hpp"
#include "support/child_process.hpp"
#include "support/fake_notify.hpp"
#include "support/free_port.hpp"
#include "support/http_client.hpp"
#include "support/temp_dir.hpp"

#include <sys/stat.h>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using std::chrono::seconds;
using ulw::test::ChildProcess;
using ulw::test::HttpClient;
using ulw::test::ScratchDatabase;
using ulw::test::TempDir;

constexpr auto kPatience = seconds(30);

// Everything a gateway needs that involves no server: the filesystem store and a local key
// set. The database URL is never dialled before the configuration is checked.
class GatewayConfigTest : public ::testing::Test {
protected:
    GatewayConfigTest() {
        auto key = devtoken::DevKey::generate();
        EXPECT_TRUE(key);
        key_.emplace(std::move(*key));
        jwks_ = files_.path() / "jwks.json";
        std::ofstream(jwks_) << key_->public_jwks();
    }

    [[nodiscard]] std::string token(std::string subject) const {
        const os::SystemClock clock;
        return *key_->mint({.issuer = "ulw-test",
                            .audience = "askedin-platform",
                            .subject = std::move(subject),
                            .email = {},
                            .ttl = seconds(600)},
                           clock.wall_now());
    }

    [[nodiscard]] std::vector<std::string> base_env() const {
        return {"ULW_STORAGE=fs", "ULW_FS_ROOT=" + (files_.path() / "store").string(),
                "ULW_DATABASE_URL=postgresql://ulw:hunter2@127.0.0.1:1/ulw",
                "ULW_DEV_JWKS_FILE=" + jwks_.string(), "JWT_ISSUER=ulw-test"};
    }

    // Runs the gateway to its exit; `args` follow the program name.
    static std::pair<int, std::string> run(const std::vector<std::string>& args,
                                           const std::vector<std::string>& env) {
        std::vector<std::string> argv{ULW_GATEWAY_BIN};
        argv.insert(argv.end(), args.begin(), args.end());
        const auto child = ChildProcess::start(argv, env);
        EXPECT_NE(child, nullptr);
        const auto code = child->wait_exit(kPatience);
        return {code.value_or(-1), child->output()};
    }

    TempDir files_{"ulw-gateway-config"};
    std::optional<devtoken::DevKey> key_;
    fs::path jwks_;
};

TEST_F(GatewayConfigTest, VersionNamesTheReleaseAndTheCommit) {
    const auto [code, output] = run({"--version"}, {});
    EXPECT_EQ(code, 0);
    const auto info = core::build_info();
    EXPECT_EQ(output, "gateway_server " + std::string(info.version) + " (" +
                          std::string(info.git_sha) + ")\n");
}

TEST_F(GatewayConfigTest, BadConfigurationExitsTwoAndNamesTheCulprit) {
    auto env = base_env();
    env.emplace_back("ULW_CHUNK_SIZE=1048576");
    const auto [code, output] = run({}, env);
    EXPECT_EQ(code, 2);
    EXPECT_NE(output.find(R"("event":"configuration refused","source":"ULW_CHUNK_SIZE")"),
              std::string::npos)
        << output;

    const auto [flag_code, flag_output] = run({"--no-such-flag=1"}, base_env());
    EXPECT_EQ(flag_code, 2);
    EXPECT_NE(flag_output.find("--no-such-flag"), std::string::npos) << flag_output;
}

// libpq quotes the token it cannot parse, which here is the password, and a failed connection
// names parts of the string; neither may reach the log.
TEST_F(GatewayConfigTest, NoPasswordReachesTheLogWhenTheDatabaseUrlIsBadOrUnreachable) {
    auto env = base_env();
    std::erase_if(env, [](const std::string& e) { return e.starts_with("ULW_DATABASE_URL="); });
    auto malformed = env;
    malformed.emplace_back("ULW_DATABASE_URL=postgresql://ulw:Sup3r%Secret@127.0.0.1:1/ulw");
    const auto [code, output] = run({}, malformed);
    EXPECT_NE(code, 0);
    EXPECT_EQ(output.find("Sup3r"), std::string::npos) << output;

    auto unreachable = env;
    unreachable.emplace_back("ULW_DATABASE_URL=postgresql://ulw:Sup3rSecret@127.0.0.1:1/ulw");
    unreachable.push_back("ULW_LISTEN_PORT=" + std::to_string(ulw::test::free_port()));
    const auto gateway = ChildProcess::start({ULW_GATEWAY_BIN}, unreachable);
    ASSERT_NE(gateway, nullptr);
    ASSERT_TRUE(
        gateway->wait_for_output(R"("event":"dependency down","dependency":"database")", kPatience))
        << gateway->output();
    gateway->signal(SIGTERM);
    EXPECT_EQ(gateway->wait_exit(kPatience), 0);
    EXPECT_EQ(gateway->output().find("Sup3r"), std::string::npos) << gateway->output();
}

// Each of these used to pass the check and fail the start with exit 1, which systemd restarts
// every two seconds; refused as configuration, they exit 2 and stay down.
TEST_F(GatewayConfigTest, TheCheckRefusesWhatWouldOtherwiseFailTheStart) {
    const std::vector<std::pair<std::string, std::string>> cases{
        {"ULW_DATABASE_URL", "postgresql://ulw:bad%zz@db/ulw"},
        {"ULW_DEV_JWKS_FILE", "/nonexistent/jwks.json"},
        {"ULW_STORAGE", "minio"},
    };
    for (const auto& [name, value] : cases) {
        auto env = base_env();
        std::erase_if(env, [&](const std::string& e) { return e.starts_with(name + "="); });
        env.push_back(name + "=" + value);
        if (name == "ULW_STORAGE") {
            std::erase_if(env, [](const std::string& e) { return e.starts_with("ULW_STORAGE="); });
            env.emplace_back("ULW_STORAGE=minio");
            env.emplace_back("ULW_S3_ENDPOINT=127.0.0.1:9000");
            env.emplace_back("ULW_BUCKET=b");
        }
        const auto [code, output] = run({"--check-config"}, env);
        EXPECT_EQ(code, 2) << name << ": " << output;
        EXPECT_NE(output.find(R"("event":"configuration refused")"), std::string::npos) << output;
    }
}

TEST_F(GatewayConfigTest, TheConnectionStringIsRefusedOnTheCommandLine) {
    const auto [code, output] = run({"--database-url=postgresql://u:pw@h/db"}, base_env());
    EXPECT_EQ(code, 2);
    EXPECT_EQ(output.find("pw@"), std::string::npos) << output;
}

TEST_F(GatewayConfigTest, AnAdmissionLimitTheDescriptorLimitCannotBackIsRefused) {
    auto env = base_env();
    // Past (65,536 - 64) / 2, the most any soft limit the gateway sets can back.
    env.emplace_back("ULW_MAX_CONNECTIONS=40000");
    env.emplace_back("ULW_MAX_UPLOAD_SLOTS=40000");
    const auto [code, output] = run({"--check-config"}, env);
    EXPECT_EQ(code, 2);
    EXPECT_NE(output.find("descriptor budget"), std::string::npos) << output;
}

TEST_F(GatewayConfigTest, FlagsBeatTheEnvironmentWhichBeatsTheFileAndSecretsAreRedacted) {
    const fs::path file = files_.path() / "gateway.toml";
    std::ofstream(file) << "[listen]\nport = 1111\nreactor = \"epoll\"\n"
                        << "[limits]\nmax_connections = 100\n";
    auto env = base_env();
    env.emplace_back("ULW_LISTEN_PORT=2222");
    env.emplace_back("ULW_MAX_CONNECTIONS=200");
    const auto [code, output] =
        run({"--config", file.string(), "--listen-port=3333", "--check-config"}, env);
    ASSERT_EQ(code, 0) << output;
    EXPECT_NE(output.find(R"("name":"ULW_LISTEN_PORT","value":"3333","from":"cli")"),
              std::string::npos)
        << output;
    EXPECT_NE(output.find(R"("name":"ULW_MAX_CONNECTIONS","value":"200","from":"env")"),
              std::string::npos);
    EXPECT_NE(output.find(R"("name":"ULW_REACTOR","value":"epoll","from":"file")"),
              std::string::npos);
    EXPECT_NE(output.find(R"("name":"ULW_DATABASE_URL","value":"<redacted>","from":"env")"),
              std::string::npos);
    EXPECT_EQ(output.find("hunter2"), std::string::npos);
}

TEST_F(GatewayConfigTest, AFileOthersCanReadMayNotHoldTheConnectionString) {
    const fs::path file = files_.path() / "gateway.toml";
    std::ofstream(file) << "[database]\nurl = \"postgresql://ulw:hunter2@db/ulw\"\n";
    ASSERT_EQ(::chmod(file.c_str(), 0644), 0);
    auto env = base_env();
    std::erase_if(env, [](const std::string& e) { return e.starts_with("ULW_DATABASE_URL="); });
    const auto [code, output] = run({"--config", file.string(), "--check-config"}, env);
    EXPECT_EQ(code, 2);
    EXPECT_NE(output.find("0400 or 0600"), std::string::npos) << output;
    ASSERT_EQ(::chmod(file.c_str(), 0600), 0);
    EXPECT_EQ(run({"--config", file.string(), "--check-config"}, env).first, 0);
}

// Against a scratch database, as the service manager runs it.
class GatewayProcessTest : public GatewayConfigTest {
protected:
    void SetUp() override {
        ScratchDatabase::open(db_);
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        port_ = ulw::test::free_port();
        ASSERT_NE(port_, 0);
        notify_path_ = (files_.path() / "notify").string();
        manager_ = std::make_unique<ulw::test::FakeNotifySocket>(notify_path_);
        ASSERT_TRUE(manager_->bound());
        auto env = base_env();
        std::erase_if(env, [](const std::string& e) { return e.starts_with("ULW_DATABASE_URL="); });
        env.push_back("ULW_DATABASE_URL=" + db_->conninfo());
        env.push_back("ULW_LISTEN_PORT=" + std::to_string(port_));
        env.push_back("NOTIFY_SOCKET=" + notify_path_);
        // Two seconds, so the loop pings every second, on its idle tick.
        env.emplace_back("WATCHDOG_USEC=2000000");
        if (const char* reactor = std::getenv("ULW_REACTOR")) {
            env.push_back("ULW_REACTOR=" + std::string(reactor));
        }
        gateway_ = ChildProcess::start({ULW_GATEWAY_BIN}, env);
        ASSERT_NE(gateway_, nullptr);
    }

    [[nodiscard]] std::optional<ulw::test::HttpResponse> get(std::string_view path) const {
        HttpClient c({.port = port_, .tls = nullptr});
        return c.request("GET", path, "");
    }

    // Polls, a request at a time, until the answer satisfies `pred`.
    template <class Pred> [[nodiscard]] bool until(Pred pred) const {
        const auto deadline = std::chrono::steady_clock::now() + kPatience;
        while (std::chrono::steady_clock::now() < deadline) {
            if (pred()) {
                return true;
            }
            static_cast<void>(manager_->receive(std::chrono::milliseconds(100)));
        }
        return false;
    }

    std::unique_ptr<ScratchDatabase> db_;
    std::uint16_t port_ = 0;
    std::string notify_path_;
    std::unique_ptr<ulw::test::FakeNotifySocket> manager_;
    std::unique_ptr<ChildProcess> gateway_;
};

TEST_F(GatewayProcessTest, ReadyForTheManagerThenForTrafficOnceTheProbeAnswers) {
    ASSERT_TRUE(manager_->wait_for("READY=1", kPatience)) << gateway_->output();
    EXPECT_EQ(get("/healthz")->status, 200);
    ASSERT_TRUE(until([&] {
        const auto r = get("/readyz");
        return r && r->status == 200;
    })) << gateway_->output();
    EXPECT_TRUE(manager_->wait_for("WATCHDOG=1", kPatience));
    const auto metrics = get("/metrics");
    ASSERT_TRUE(metrics && metrics->status == 200);
    EXPECT_NE(metrics->body.find("\nready 1\n"), std::string::npos);
    EXPECT_NE(metrics->body.find("\njobs_oldest_queued_seconds 0\n"), std::string::npos);
    EXPECT_NE(metrics->body.find("build_info{version=\""), std::string::npos);
    gateway_->signal(SIGTERM);
    EXPECT_EQ(gateway_->wait_exit(kPatience), 0) << gateway_->output();
}

TEST_F(GatewayProcessTest, TheOldestDueJobsAgeIsExported) {
    auto conn = db_->session();
    ASSERT_TRUE(conn.exec("INSERT INTO videos (id, owner_id, title, state) VALUES "
                          "('01890a5d-ac96-774b-bcce-b302099a8057', 'alice', 't', 'processing')"));
    ASSERT_TRUE(conn.exec("INSERT INTO jobs (video_id, kind, run_after) VALUES "
                          "('01890a5d-ac96-774b-bcce-b302099a8057', 'transcode', "
                          "now() - interval '90 seconds')"));
    ASSERT_TRUE(until([&] {
        const auto r = get("/metrics");
        const std::string_view name = "\njobs_oldest_queued_seconds ";
        const std::size_t at = r ? r->body.find(name) : std::string::npos;
        if (at == std::string::npos) {
            return false;
        }
        const std::string value =
            r->body.substr(at + name.size(), r->body.find('\n', at + 1) - at - name.size());
        return std::stoi(value) >= 90;
    })) << gateway_->output();
    gateway_->signal(SIGTERM);
    EXPECT_EQ(gateway_->wait_exit(kPatience), 0);
}

TEST_F(GatewayProcessTest, SigtermLetsTheRequestInFlightFinishThenExitsZero) {
    ASSERT_TRUE(until([&] {
        const auto r = get("/readyz");
        return r && r->status == 200;
    })) << gateway_->output();
    const std::string body = R"({"filename":"a.mp4","size_bytes":10,"content_type":"video/mp4"})";
    HttpClient busy({.port = port_, .tls = nullptr});
    ASSERT_TRUE(busy.send_raw(
        "POST /api/v1/uploads HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer " + token("alice") +
        "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body.substr(0, 10)));
    // Buffered body bytes show the gateway holds the request.
    ASSERT_TRUE(until([&] {
        const auto r = get("/metrics");
        return r && r->body.find("\nbuffer_bytes_in_use 0\n") == std::string::npos;
    }));
    gateway_->signal(SIGTERM);
    ASSERT_TRUE(manager_->wait_for("STOPPING=1", kPatience)) << gateway_->output();
    // New connections are no longer taken.
    EXPECT_FALSE(get("/healthz").has_value());
    ASSERT_TRUE(busy.send_raw(body.substr(10)));
    const auto r = busy.read_response();
    ASSERT_TRUE(r) << gateway_->output();
    EXPECT_EQ(r->status, 201);
    EXPECT_EQ(r->header("connection"), "close");
    EXPECT_EQ(gateway_->wait_exit(kPatience), 0) << gateway_->output();
    EXPECT_NE(gateway_->output().find(R"("event":"drained")"), std::string::npos);
}

} // namespace
