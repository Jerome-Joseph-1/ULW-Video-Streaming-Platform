#include "postgres_harness.hpp"

#include "infra/postgres/migrator.hpp"

#include "libpq_handles.hpp"

#include <sys/wait.h>

#include <array>
#include <cstdlib>
#include <fcntl.h>
#include <format>
#include <gtest/gtest.h>
#include <print>
#include <random>
#include <spawn.h>
#include <string_view>
#include <unistd.h>

namespace ulw::test {

namespace {

constexpr std::string_view kDefaultUrl =
    "postgresql://postgres:testtest123@127.0.0.1:55432/postgres";

std::string single_quoted(std::string_view value) {
    std::string out = "'";
    for (const char c : value) {
        if (c == '\'' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    out += '\'';
    return out;
}

std::string random_suffix() {
    std::random_device device;
    std::uniform_int_distribution<std::uint64_t> any;
    return std::format("{:016x}", any(device));
}

// Apart from "ulw-test", which the pool tests give their sessions and then terminate.
constexpr infra::postgres::SessionSettings kFixtureSession{.application_name = "ulw-test-fixture",
                                                           .statement_timeout = core::Millis{0}};

} // namespace

std::string admin_url() {
    const char* configured = std::getenv("ULW_TEST_DATABASE_URL");
    return configured != nullptr && *configured != '\0' ? configured : std::string{kDefaultUrl};
}

std::string with_database(const std::string& url, const std::string& database) {
    char* error = nullptr;
    const infra::postgres::ConninfoHandle options{PQconninfoParse(url.c_str(), &error)};
    PQfreemem(error);
    std::string out;
    for (const PQconninfoOption* o = options.get(); o != nullptr && o->keyword != nullptr; ++o) {
        if (o->val != nullptr && std::string_view{o->keyword} != "dbname") {
            out += std::format("{}={} ", o->keyword, single_quoted(o->val));
        }
    }
    out += "dbname=" + single_quoted(database);
    return out;
}

void ScratchDatabase::open(std::unique_ptr<ScratchDatabase>& out, Schema schema) {
    const bool configured = std::getenv("ULW_TEST_DATABASE_URL") != nullptr;
    auto admin = infra::postgres::SyncConnection::open(admin_url(), kFixtureSession);
    if (!admin) {
        if (!configured) {
            GTEST_SKIP() << "no Postgres at " << kDefaultUrl
                         << " (deploy/local/compose.yaml starts one): " << admin.error().message;
        }
        FAIL() << "ULW_TEST_DATABASE_URL is set but unreachable: " << admin.error().message;
    }
    const std::string name = "ulw_test_" + random_suffix();
    const std::string create = "CREATE DATABASE " + name;
    ASSERT_TRUE(admin->run_script(create.c_str()));
    out = std::make_unique<ScratchDatabase>(std::move(*admin), name,
                                            with_database(admin_url(), name));
    if (schema == Schema::Migrated) {
        auto migrator = infra::postgres::Migrator::connect(out->conninfo());
        ASSERT_TRUE(migrator) << migrator.error().message;
        const auto applied = migrator->apply(infra::postgres::bundled_migrations());
        ASSERT_TRUE(applied) << applied.error().message;
    }
}

ScratchDatabase::ScratchDatabase(infra::postgres::SyncConnection admin, std::string name,
                                 std::string conninfo)
    : admin_(std::move(admin)), name_(std::move(name)), conninfo_(std::move(conninfo)) {}

ScratchDatabase::~ScratchDatabase() {
    const std::string drop = "DROP DATABASE IF EXISTS " + name_ + " WITH (FORCE)";
    EXPECT_TRUE(admin_.run_script(drop.c_str()));
}

infra::postgres::SyncConnection ScratchDatabase::session() const {
    auto conn = infra::postgres::SyncConnection::open(conninfo_, kFixtureSession);
    if (!conn) {
        // Nothing sensible can follow; a test only asks for a session on a database it made.
        std::println(stderr, "session on the scratch database: {}", conn.error().message);
        std::abort();
    }
    return std::move(*conn);
}

ProcessResult run_process(const std::vector<std::string>& argv,
                          const std::vector<std::string>& env) {
    std::array<int, 2> pipe_fds{};
    if (::pipe2(pipe_fds.data(), O_CLOEXEC) != 0) {
        return {};
    }
    posix_spawn_file_actions_t actions{};
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);

    std::vector<std::string> args = argv;
    std::vector<char*> arg_ptrs;
    arg_ptrs.reserve(args.size() + 1);
    for (std::string& a : args) {
        arg_ptrs.push_back(a.data());
    }
    arg_ptrs.push_back(nullptr);
    std::vector<std::string> vars;
    for (char** e = ::environ; *e != nullptr; ++e) {
        vars.emplace_back(*e);
    }
    vars.insert(vars.end(), env.begin(), env.end());
    std::vector<char*> var_ptrs;
    var_ptrs.reserve(vars.size() + 1);
    for (std::string& v : vars) {
        var_ptrs.push_back(v.data());
    }
    var_ptrs.push_back(nullptr);

    pid_t pid = 0;
    const int spawned =
        ::posix_spawnp(&pid, arg_ptrs[0], &actions, nullptr, arg_ptrs.data(), var_ptrs.data());
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipe_fds[1]);
    ProcessResult result;
    if (spawned != 0) {
        ::close(pipe_fds[0]);
        return result;
    }
    std::array<char, 4096> buf{};
    for (;;) {
        const ssize_t n = ::read(pipe_fds[0], buf.data(), buf.size());
        if (n <= 0) {
            break;
        }
        result.output.append(buf.data(), static_cast<std::size_t>(n));
    }
    ::close(pipe_fds[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

std::optional<std::string> postgres_container() {
    const char* configured = std::getenv("ULW_TEST_PG_CONTAINER");
    std::string name = configured != nullptr && *configured != '\0' ? configured : "ulw-pg";
    const ProcessResult running =
        run_process({"docker", "inspect", "--format", "{{.State.Running}}", name});
    if (running.exit_code != 0 || !running.output.starts_with("true")) {
        return std::nullopt;
    }
    return name;
}

PausedServer::PausedServer(std::string container) : container_(std::move(container)) {
    paused_ = run_process({"docker", "pause", container_}).exit_code == 0;
}

PausedServer::~PausedServer() {
    if (paused_) {
        EXPECT_EQ(run_process({"docker", "unpause", container_}).exit_code, 0);
    }
}

std::string scalar(infra::postgres::SyncConnection& conn, infra::postgres::Sql sql,
                   const infra::postgres::Params& params) {
    auto result = conn.exec(sql, params);
    if (!result) {
        ADD_FAILURE() << result.error().message;
        return {};
    }
    if (result->rows() == 0) {
        return {};
    }
    return std::string{result->get(0, 0).value_or("")};
}

} // namespace ulw::test
