// gateway_server as the service manager runs it: its version, its refusal of bad
// configuration with exit code 2, the layering of file, environment and flags, readiness that
// follows the database, the notify protocol, and the SIGTERM drain.
#include "core/version.hpp"
#include "os/system_clock.hpp"
#include "os/unique_fd.hpp"

#include "devtoken/dev_key.hpp"
#include "postgres_harness.hpp"
#include "support/child_process.hpp"
#include "support/core_limit.hpp"
#include "support/fake_notify.hpp"
#include "support/http_client.hpp"
#include "support/reserve_port.hpp"
#include "support/temp_dir.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <pwd.h>
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
                "ULW_DEV_JWKS_FILE=" + jwks_.string(), "JWT_ISSUER=ulw-test",
                // Developers and some runners start tests as root; the refusal has tests below.
                "ULW_ALLOW_ROOT=1"};
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

// Started as root, the gateway either becomes the user it is told to or refuses to start. Only
// root can show either; CI runs these under sudo.
class GatewayAsRoot : public GatewayConfigTest {
protected:
    void SetUp() override {
        if (::geteuid() != 0) {
            GTEST_SKIP() << "needs root";
        }
    }

    [[nodiscard]] std::vector<std::string> env_without_allow_root() const {
        auto env = base_env();
        std::erase(env, std::string("ULW_ALLOW_ROOT=1"));
        // CI runs these once per reactor.
        // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts.
        if (const char* reactor = std::getenv("ULW_REACTOR")) {
            env.push_back("ULW_REACTOR=" + std::string(reactor));
        }
        return env;
    }
};

TEST_F(GatewayAsRoot, RootWithNoUserToBecomeIsRefusedAsConfiguration) {
    const auto [code, output] = run({"--check-config"}, env_without_allow_root());
    EXPECT_EQ(code, 2);
    EXPECT_NE(output.find(R"("source":"ULW_RUN_AS_USER")"), std::string::npos) << output;
}

// A port under 1024 needs root to bind, which the gateway no longer has once it serves.
std::optional<std::uint16_t> free_privileged_port() {
    for (std::uint16_t port = 1023; port >= 900; --port) {
        const os::UniqueFd fd{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
        if (!fd) {
            return std::nullopt;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        // bind() takes every address family through the generic sockaddr header.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0) {
            return port;
        }
    }
    return std::nullopt;
}

// Whether this test binary, and so the gateway beside it, is built with AddressSanitizer.
constexpr bool kBuiltWithAsan =
#if defined(__SANITIZE_ADDRESS__)
    true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    true;
#else
    false;
#endif
#else
    false;
#endif

TEST_F(GatewayAsRoot, BindsAPrivilegedPortThenServesAsTheUserItNames) {
    const passwd* nobody = ::getpwnam("nobody");
    const auto port = free_privileged_port();
    if (nobody == nullptr || !port) {
        GTEST_SKIP() << "no nobody user, or no free port under 1024";
    }
    auto env = env_without_allow_root();
    env.emplace_back("ULW_RUN_AS_USER=nobody");
    env.push_back("ULW_LISTEN_PORT=" + std::to_string(*port));
    if (kBuiltWithAsan) {
        // LeakSanitizer checks at exit by stopping the process's threads with ptrace from a
        // helper it clones. Changing uid leaves the process non-dumpable, and under Yama
        // (ptrace_scope 1, as on Ubuntu) the kernel then refuses the attach: CI's run logged
        // "LeakSanitizer has encountered a fatal error" after "drained", and exit 1. The drop
        // is the point of the test and stays; the same code paths are leak-checked by every
        // test that runs the gateway without root.
        // The child gets this environment and nothing else, so what CI set is carried over.
        // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts.
        const char* inherited = std::getenv("ASAN_OPTIONS");
        env.push_back("ASAN_OPTIONS=" +
                      (inherited == nullptr ? std::string() : std::string(inherited) + ":") +
                      "detect_leaks=0");
    }
    const auto gateway = ChildProcess::start({ULW_GATEWAY_BIN}, env);
    ASSERT_NE(gateway, nullptr);
    ASSERT_TRUE(gateway->wait_for_output(R"("event":"listening")", kPatience)) << gateway->output();
    EXPECT_NE(gateway->output().find(R"("event":"dropped root")"), std::string::npos);
    std::ifstream status("/proc/" + std::to_string(gateway->pid()) + "/status");
    std::string line;
    while (std::getline(status, line) && !line.starts_with("Uid:")) {
    }
    const std::string uid = std::to_string(nobody->pw_uid);
    EXPECT_EQ(line, "Uid:\t" + uid + "\t" + uid + "\t" + uid + "\t" + uid);
    HttpClient c({.port = *port, .tls = nullptr});
    const auto r = c.request("GET", "/healthz", "");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    gateway->signal(SIGTERM);
    EXPECT_EQ(gateway->wait_exit(kPatience), 0) << gateway->output();
}

// A crash would otherwise write the database password, the store keys and live bearer tokens to
// disk.
TEST_F(GatewayConfigTest, TheServerCanWriteNoCoreFile) {
    const ulw::test::RaisedCoreLimit limit;
    if (!limit.raised()) {
        GTEST_SKIP() << "the hard core limit is 0 here; there is nothing to lower";
    }
    auto started = ulw::test::start_until_listening(
        [&] {
            auto env = base_env();
            env.push_back("ULW_LISTEN_PORT=" + std::to_string(ulw::test::reserve_port()));
            return ChildProcess::start({ULW_GATEWAY_BIN}, env);
        },
        R"("event":"listening")", kPatience);
    const auto gateway = std::move(started.process);
    ASSERT_NE(gateway, nullptr);
    ASSERT_TRUE(started.ready) << gateway->output();
    EXPECT_EQ(ulw::test::core_limit_of(gateway->pid()), std::optional<std::string>("0 0"));
    gateway->signal(SIGTERM);
    EXPECT_EQ(gateway->wait_exit(kPatience), 0) << gateway->output();
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
    auto started = ulw::test::start_until_listening(
        [&] {
            auto with_port = unreachable;
            with_port.push_back("ULW_LISTEN_PORT=" + std::to_string(ulw::test::reserve_port()));
            return ChildProcess::start({ULW_GATEWAY_BIN}, with_port);
        },
        R"("event":"dependency down","dependency":"database")", kPatience);
    const auto gateway = std::move(started.process);
    ASSERT_NE(gateway, nullptr);
    ASSERT_TRUE(started.ready) << gateway->output();
    gateway->signal(SIGTERM);
    EXPECT_EQ(gateway->wait_exit(kPatience), 0);
    EXPECT_EQ(gateway->output().find("Sup3r"), std::string::npos) << gateway->output();
}

// Each of these used to pass the check and fail the start with exit 1, which systemd restarts
// every two seconds; refused as configuration, they exit 2 and stay down.
TEST_F(GatewayConfigTest, TheCheckRefusesWhatWouldOtherwiseFailTheStart) {
    // Each case replaces base_env()'s variables of the same names.
    const std::vector<std::vector<std::string>> cases{
        {"ULW_DATABASE_URL=postgresql://ulw:bad%zz@db/ulw"},
        {"ULW_DEV_JWKS_FILE=/nonexistent/jwks.json"},
        {"ULW_STORAGE=minio", "ULW_S3_ENDPOINT=127.0.0.1:9000", "ULW_BUCKET=b"},
    };
    for (const auto& changes : cases) {
        auto env = base_env();
        for (const std::string& change : changes) {
            const std::string name = change.substr(0, change.find('=') + 1);
            std::erase_if(env, [&](const std::string& e) { return e.starts_with(name); });
            env.push_back(change);
        }
        const std::string& name = changes.front();
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
        notify_path_ = (files_.path() / "notify").string();
        manager_ = std::make_unique<ulw::test::FakeNotifySocket>(notify_path_);
        ASSERT_TRUE(manager_->bound());
        auto env = base_env();
        std::erase_if(env, [](const std::string& e) { return e.starts_with("ULW_DATABASE_URL="); });
        env.push_back("ULW_DATABASE_URL=" + db_->conninfo());
        env.push_back("NOTIFY_SOCKET=" + notify_path_);
        // Two seconds, so the loop pings every second, on its idle tick.
        env.emplace_back("WATCHDOG_USEC=2000000");
        if (const char* reactor = std::getenv("ULW_REACTOR")) {
            env.push_back("ULW_REACTOR=" + std::string(reactor));
        }
        auto started = ulw::test::start_until_listening(
            [&] {
                port_ = ulw::test::reserve_port();
                auto with_port = env;
                with_port.push_back("ULW_LISTEN_PORT=" + std::to_string(port_));
                return port_ == 0 ? nullptr : ChildProcess::start({ULW_GATEWAY_BIN}, with_port);
            },
            R"("event":"listening")", kPatience);
        gateway_ = std::move(started.process);
        ASSERT_NE(gateway_, nullptr);
        ASSERT_TRUE(started.ready) << gateway_->output();
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
        // NaN until the first probe has asked.
        return value != "NaN" && std::stoi(value) >= 90;
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
