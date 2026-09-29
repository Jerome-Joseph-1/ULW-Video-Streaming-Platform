// The sandbox as the transcoder uses it: ulw_sandbox started by run_sandboxed, with ordinary
// programs standing in for ffmpeg so each layer can be probed directly.
#include "infra/ffmpeg/transcoder.hpp"
#include "os/system_clock.hpp"
#include "os/unique_fd.hpp"

#include "process.hpp"
#include "support/temp_dir.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <pthread.h>
#include <stop_token>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;
using infra::ffmpeg::Args;
using infra::ffmpeg::ChildExit;
using infra::ffmpeg::Ending;
using infra::ffmpeg::Limits;
using infra::ffmpeg::Sandbox;

constexpr std::uint64_t kGiB = std::uint64_t{1} << 30U;

// Is a process with exactly this command line running anywhere on the host? Zombies count
// as gone.
bool host_runs(const std::vector<std::string>& argv) {
    std::string wanted;
    for (const std::string& arg : argv) {
        wanted += arg;
        wanted.push_back('\0');
    }
    for (const auto& entry : fs::directory_iterator("/proc")) {
        std::ifstream cmdline(entry.path() / "cmdline", std::ios::binary);
        std::string text;
        std::getline(cmdline, text, '\n');
        std::ifstream stat(entry.path() / "stat");
        std::string state;
        std::getline(stat, state);
        if (text == wanted && state.find(") Z ") == std::string::npos) {
            return true;
        }
    }
    return false;
}

class SandboxTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (const auto refused = infra::ffmpeg::check_sandbox(kHelper, writable_.path(), clock_)) {
            GTEST_SKIP() << "this host refuses the sandbox: " << *refused;
        }
    }

    ChildExit run(const Args& args, Limits limits = {}, const std::stop_token& stop = {}) {
        if (limits.writable.empty()) {
            limits.writable = writable_.path();
        }
        if (limits.address_space_bytes == 0) {
            limits.address_space_bytes = kGiB;
        }
        if (limits.cpu == core::Seconds{}) {
            limits.cpu = core::Seconds{30};
        }
        if (limits.wall == core::Millis{}) {
            limits.wall = core::Millis{30'000};
        }
        stdout_.clear();
        auto child = infra::ffmpeg::run_sandboxed(
            Sandbox{.helper = kHelper, .environment = {"PATH=/usr/bin:/bin"}}, limits, args, clock_,
            [this](std::string_view bytes) {
                stdout_.append(bytes);
                if (on_stdout_) {
                    on_stdout_(stdout_);
                }
            },
            stop);
        EXPECT_TRUE(child) << child.error();
        return child.value_or(ChildExit{});
    }

    const fs::path kHelper{ULW_SANDBOX_BIN};
    os::SystemClock clock_;
    ulw::test::TempDir writable_{"ulw-sandbox"};
    std::string stdout_;
    std::function<void(const std::string&)> on_stdout_;
};

TEST_F(SandboxTest, WritesSucceedOnlyInsideTheWritableDirectory) {
    const ulw::test::TempDir elsewhere("ulw-sandbox-elsewhere");
    const auto child = run({"sh", "-c", R"(echo in > "$1"/f; echo out > "$2"/f)", "sh",
                            writable_.path().string(), elsewhere.path().string()});
    EXPECT_NE(child.exit_code, 0);
    EXPECT_NE(child.stderr_tail.find("Read-only file system"), std::string::npos)
        << child.stderr_tail;
    EXPECT_TRUE(fs::exists(writable_.path() / "f"));
    EXPECT_FALSE(fs::exists(elsewhere.path() / "f"));
}

TEST_F(SandboxTest, HasNoNetworkNotEvenLoopback) {
    const os::UniqueFd listener(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    ASSERT_TRUE(listener);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof addr;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API's own cast.
    auto* const any = reinterpret_cast<sockaddr*>(&addr);
    ASSERT_EQ(::bind(listener.get(), any, sizeof addr), 0);
    ASSERT_EQ(::listen(listener.get(), 1), 0);
    ASSERT_EQ(::getsockname(listener.get(), any, &len), 0);
    const std::string port = std::to_string(ntohs(addr.sin_port));

    // From out here the listener answers, so a refusal in there is the namespace's doing.
    const os::UniqueFd probe(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    ASSERT_TRUE(probe);
    ASSERT_EQ(::connect(probe.get(), any, sizeof addr), 0);

    const auto child = run({"bash", "-c", "exec 3<>/dev/tcp/127.0.0.1/" + port});
    EXPECT_NE(child.exit_code, 0);
    const auto interfaces = run({"cat", "/proc/self/net/dev"});
    EXPECT_EQ(interfaces.exit_code, 0);
    EXPECT_NE(stdout_.find("lo:"), std::string::npos);
    // Header lines plus loopback, and nothing else.
    EXPECT_EQ(std::ranges::count(stdout_, '\n'), 3) << stdout_;
}

TEST_F(SandboxTest, LimitsReachTheProgram) {
    const auto child =
        run({"sh", "-c", "ulimit -v; ulimit -t; ulimit -c"},
            {.writable = {}, .address_space_bytes = 2 * kGiB, .cpu = core::Seconds{7}, .wall = {}});
    EXPECT_EQ(child.exit_code, 0);
    EXPECT_EQ(stdout_, "2097152\n7\n0\n");
}

TEST_F(SandboxTest, TheProgramHoldsNoCapabilitiesAndCannotRegainThem) {
    const auto child = run({"sh", "-c",
                            "grep -E '^Cap(Eff|Prm|Bnd)' /proc/self/status; "
                            "grep NoNewPrivs /proc/self/status"});
    EXPECT_EQ(child.exit_code, 0);
    EXPECT_EQ(stdout_, "CapPrm:\t0000000000000000\nCapEff:\t0000000000000000\n"
                       "CapBnd:\t0000000000000000\nNoNewPrivs:\t1\n");
}

TEST_F(SandboxTest, TheProgramGetsOnlyTheGivenEnvironment) {
    const auto child = run({"env"});
    EXPECT_EQ(child.exit_code, 0);
    EXPECT_EQ(stdout_, "PATH=/usr/bin:/bin\n");
}

TEST_F(SandboxTest, StdinIsEmptyAndNoOtherDescriptorLeaks) {
    // Not close-on-exec, as a descriptor from a careless library would be.
    const os::UniqueFd leaky(::open("/dev/null", O_RDONLY));
    ASSERT_TRUE(leaky);
    const auto child = run({"sh", "-c", "cat; ls /proc/self/fd"});
    EXPECT_EQ(child.exit_code, 0);
    // 0-2, and 3 for the directory ls is reading.
    EXPECT_EQ(stdout_, "0\n1\n2\n3\n");
}

TEST_F(SandboxTest, TheWallClockDeadlineTerminatesAnOverrun) {
    const auto started = std::chrono::steady_clock::now();
    const auto child =
        run({"sleep", "30"},
            {.writable = {}, .address_space_bytes = 0, .cpu = {}, .wall = core::Millis{200}});
    EXPECT_EQ(child.ending, Ending::TimedOut);
    EXPECT_EQ(child.exit_code, 128 + SIGTERM);
    EXPECT_LT(std::chrono::steady_clock::now() - started, infra::ffmpeg::kTerminationGrace);
}

// A descendant that left the program's session and process group, with the program's stdout
// still open: `until` holds the program back until that descendant is sleeping.
constexpr std::string_view kDetachedSleeper =
    "setsid sleep $0 & until [ \"$(cat /proc/$!/comm 2>/dev/null)\" = sleep ]; do :; done; "
    "echo up; ";

TEST_F(SandboxTest, NothingTheProgramStartedOutlivesIt) {
    const auto started = std::chrono::steady_clock::now();
    const auto child = run({"sh", "-c", std::string(kDetachedSleeper) + "exit 0", "20.25"},
                           {.writable = {}, .address_space_bytes = 0, .cpu = {}, .wall = {}});
    EXPECT_EQ(child.ending, Ending::Exited);
    EXPECT_EQ(child.exit_code, 0);
    EXPECT_EQ(stdout_, "up\n");
    // Not the sleeper's 20 s, nor the 30 s wall deadline.
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(10));
    EXPECT_FALSE(host_runs({"sleep", "20.25"}));
}

TEST_F(SandboxTest, TheWallClockDeadlineHoldsWithADetachedDescendantAlive) {
    const auto started = std::chrono::steady_clock::now();
    const auto child =
        run({"sh", "-c", std::string(kDetachedSleeper) + "exec sleep 21.5", "20.75"},
            {.writable = {}, .address_space_bytes = 0, .cpu = {}, .wall = core::Millis{1000}});
    EXPECT_EQ(child.ending, Ending::TimedOut);
    EXPECT_EQ(child.exit_code, 128 + SIGTERM);
    EXPECT_EQ(stdout_, "up\n");
    EXPECT_LT(std::chrono::steady_clock::now() - started,
              core::Millis{1000} + infra::ffmpeg::kTerminationGrace);
    EXPECT_FALSE(host_runs({"sleep", "20.75"}));
    EXPECT_FALSE(host_runs({"sleep", "21.5"}));
}

TEST_F(SandboxTest, TheProgramSeesOnlyItsOwnPidNamespace) {
    // pid 1 is the helper, the program its child; the worker, and everything else on the
    // host, is not in this /proc at all.
    const auto child = run(
        {"sh", "-c", "echo $$; cat /proc/1/comm; test -e /proc/$0", std::to_string(::getpid())});
    EXPECT_EQ(child.exit_code, 1);
    EXPECT_EQ(stdout_, "2\nulw_sandbox\n");
}

TEST_F(SandboxTest, AStopRequestTerminatesTheProgramEvenWhenWeBlockSigterm) {
    // The worker blocks SIGTERM in all its threads; the child must not inherit that, or only
    // SIGKILL after the grace period would end it.
    sigset_t term;
    sigemptyset(&term);
    sigaddset(&term, SIGTERM);
    sigset_t previous;
    ASSERT_EQ(::pthread_sigmask(SIG_BLOCK, &term, &previous), 0);
    std::stop_source stop;
    on_stdout_ = [&stop](const std::string& seen) {
        if (seen.find("started") != std::string::npos) {
            stop.request_stop();
        }
    };
    const auto child = run({"sh", "-c", "echo started; exec sleep 30"}, {}, stop.get_token());
    ASSERT_EQ(::pthread_sigmask(SIG_SETMASK, &previous, nullptr), 0);
    EXPECT_EQ(child.ending, Ending::Stopped);
    EXPECT_EQ(child.exit_code, 128 + SIGTERM);
}

TEST_F(SandboxTest, TheCpuLimitStopsASpinningProgram) {
    const auto child =
        run({"sh", "-c", "while :; do :; done"},
            {.writable = {}, .address_space_bytes = 0, .cpu = core::Seconds{1}, .wall = {}});
    EXPECT_EQ(child.ending, Ending::CpuExhausted);
    EXPECT_EQ(child.exit_code, 128 + SIGXCPU);
}

TEST_F(SandboxTest, TheCpuLimitStopsAProgramThatIgnoresSigxcpu) {
    const auto child =
        run({"sh", "-c", "trap '' XCPU; while :; do :; done"},
            {.writable = {}, .address_space_bytes = 0, .cpu = core::Seconds{1}, .wall = {}});
    EXPECT_EQ(child.ending, Ending::CpuExhausted);
    EXPECT_EQ(child.exit_code, 128 + SIGKILL);
}

TEST_F(SandboxTest, AFailureWithCpuTimeToSpareIsNotTheLimits) {
    const auto child =
        run({"sh", "-c", "kill -KILL $$"},
            {.writable = {}, .address_space_bytes = 0, .cpu = core::Seconds{1}, .wall = {}});
    EXPECT_EQ(child.ending, Ending::Exited);
    EXPECT_EQ(child.exit_code, 128 + SIGKILL);
    EXPECT_EQ(child.signal, SIGKILL);
}

TEST_F(SandboxTest, AnExitCodeAbove128ArrivesAsAnExitNotASignal) {
    // What ffmpeg exits with for a corrupt input; read as 128 + 55 it would requeue the file
    // until its attempts ran out instead of rejecting it.
    const auto child = run({"sh", "-c", "exit 183"});
    EXPECT_EQ(child.ending, Ending::Exited);
    EXPECT_EQ(child.exit_code, 183);
    EXPECT_EQ(child.signal, 0);
    EXPECT_EQ(infra::ffmpeg::classify(child.exit_code, child.signal, child.ending),
              core::ports::TranscodeFailure::Rejected);
}

TEST_F(SandboxTest, AProgramThatCrashesArrivesAsTheSignalThatKilledIt) {
    const auto child = run({"sh", "-c", "kill -SEGV $$"});
    EXPECT_EQ(child.ending, Ending::Exited);
    EXPECT_EQ(child.signal, SIGSEGV);
    EXPECT_EQ(child.exit_code, 128 + SIGSEGV);
    EXPECT_EQ(infra::ffmpeg::classify(child.exit_code, child.signal, child.ending),
              core::ports::TranscodeFailure::Crashed);
}

TEST_F(SandboxTest, TheAddressSpaceLimitRefusesALargeAllocation) {
    const Args allocate{"python3", "-c", "bytearray(512 << 20)"};
    // 256 MiB of address space cannot hold a 512 MiB buffer; 2 GiB can.
    EXPECT_NE(run(allocate, {.writable = {},
                             .address_space_bytes = std::uint64_t{256} << 20U,
                             .cpu = {},
                             .wall = {}})
                  .exit_code,
              0);
    EXPECT_EQ(
        run(allocate, {.writable = {}, .address_space_bytes = 2 * kGiB, .cpu = {}, .wall = {}})
            .exit_code,
        0);
}

TEST_F(SandboxTest, ReportsThePeakResidentSetOfTheProgram) {
    const auto child = run({"true"});
    EXPECT_EQ(child.exit_code, 0);
    EXPECT_GT(child.peak_rss_kib, 0U);
}

TEST_F(SandboxTest, KeepsOnlyTheTailOfALongStderr) {
    const auto child = run({"sh", "-c",
                            "i=0; while [ $i -lt 2000 ]; do echo line$i >&2; "
                            "i=$((i+1)); done"});
    EXPECT_EQ(child.exit_code, 0);
    EXPECT_GT(child.stderr_bytes, 10'000U);
    EXPECT_LE(child.stderr_tail.size(), 4096U);
    EXPECT_TRUE(child.stderr_tail.ends_with("line1998\nline1999\n"));
}

TEST_F(SandboxTest, TheHelpersOwnFailuresHaveTheirOwnCodes) {
    EXPECT_EQ(run({"no-such-program-anywhere"}).exit_code, infra::ffmpeg::kProgramNotFound);
    const auto child = run({"true"}, {.writable = writable_.path() / "missing",
                                      .address_space_bytes = 0,
                                      .cpu = {},
                                      .wall = {}});
    EXPECT_EQ(child.exit_code, infra::ffmpeg::kSandboxSetupFailed);
    EXPECT_NE(child.stderr_tail.find("writable directory"), std::string::npos);
}

TEST(SandboxCheck, NamesWhatWentWrong) {
    const os::SystemClock clock;
    const auto refused = infra::ffmpeg::check_sandbox("/nonexistent/ulw_sandbox", "/tmp", clock);
    ASSERT_TRUE(refused);
    EXPECT_NE(refused->find("/nonexistent/ulw_sandbox"), std::string::npos);
}

} // namespace
