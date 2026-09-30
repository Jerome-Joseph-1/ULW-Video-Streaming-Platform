// The sandbox as the transcoder uses it: ulw_sandbox started by run_sandboxed, with ordinary
// programs standing in for ffmpeg so each layer can be probed directly.
#include "infra/ffmpeg/transcoder.hpp"
#include "os/system_clock.hpp"
#include "os/unique_fd.hpp"

#include "live_command.hpp"
#include "process.hpp"
#include "support/temp_dir.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <grp.h>
#include <gtest/gtest.h>
#include <pthread.h>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
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
constexpr std::uint64_t kFileLimit = std::uint64_t{3} << 20U;

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

    ChildExit run(const Args& args, Limits limits = {}, const std::stop_token& stop = {},
                  int input = -1) {
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
            Sandbox{.helper = kHelper,
                    .environment = {"PATH=/usr/bin:/bin"},
                    .syscall_filter = syscall_filter_},
            limits, args, clock_,
            [this](std::string_view bytes) {
                stdout_.append(bytes);
                if (on_stdout_) {
                    on_stdout_(stdout_);
                }
            },
            stop, input);
        EXPECT_TRUE(child) << child.error();
        return child.value_or(ChildExit{});
    }

    const fs::path kHelper{ULW_SANDBOX_BIN};
    // Off here so that an ordinary shell can stand in for ffmpeg; SyscallFilterTest turns it on.
    bool syscall_filter_ = false;
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

TEST_F(SandboxTest, AFileSizeLimitReachesTheProgramAndStopsAFileGrowingPastIt) {
    const auto reported = run({"bash", "-c", "ulimit -f"}, {.writable = {},
                                                            .address_space_bytes = 0,
                                                            .cpu = {},
                                                            .wall = {},
                                                            .file_size_bytes = kFileLimit});
    EXPECT_EQ(reported.exit_code, 0);
    // bash counts ulimit -f in 1024-byte blocks.
    EXPECT_EQ(stdout_, "3072\n");

    const auto grown = run({"sh", "-c", "dd if=/dev/zero of=big bs=1M count=8 2>/dev/null"},
                           {.writable = {},
                            .address_space_bytes = 0,
                            .cpu = {},
                            .wall = {},
                            .file_size_bytes = kFileLimit});
    EXPECT_NE(grown.exit_code, 0);
    EXPECT_LE(fs::file_size(writable_.path() / "big"), kFileLimit);
}

TEST_F(SandboxTest, WithoutAFileSizeLimitAFileGrowsFreely) {
    const auto child = run({"bash", "-c", "ulimit -f"});
    EXPECT_EQ(child.exit_code, 0);
    EXPECT_EQ(stdout_, "unlimited\n");
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

TEST_F(SandboxTest, AnInputDescriptorBecomesStdinAndNothingElseLeaks) {
    std::array<int, 2> fds{};
    ASSERT_EQ(::pipe2(fds.data(), O_CLOEXEC), 0);
    const os::UniqueFd read_end(fds[0]);
    os::UniqueFd write_end(fds[1]);
    constexpr std::string_view kBytes = "ts bytes";
    ASSERT_EQ(::write(write_end.get(), kBytes.data(), kBytes.size()),
              static_cast<ssize_t>(kBytes.size()));
    write_end.reset();
    const auto child = run({"sh", "-c", "cat; ls /proc/self/fd"}, {}, {}, read_end.get());
    EXPECT_EQ(child.exit_code, 0);
    EXPECT_EQ(stdout_, "ts bytes0\n1\n2\n3\n");
}

TEST_F(SandboxTest, ASocketFromOutsideStillCarriesBytesIntoTheNamespaceWithoutNetwork) {
    std::array<int, 2> pair{};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair.data()), 0);
    const os::UniqueFd ours(pair[0]);
    const os::UniqueFd theirs(pair[1]);
    constexpr std::string_view kBytes = "from the publisher";
    ASSERT_EQ(::write(ours.get(), kBytes.data(), kBytes.size()),
              static_cast<ssize_t>(kBytes.size()));
    ASSERT_EQ(::shutdown(ours.get(), SHUT_WR), 0);
    const auto child = run({"cat"}, {}, {}, theirs.get());
    EXPECT_EQ(child.exit_code, 0);
    EXPECT_EQ(stdout_, kBytes);
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

class SyscallFilterTest : public SandboxTest {
protected:
    SyscallFilterTest() { syscall_filter_ = true; }

    const fs::path kProbe{ULW_SYSCALL_PROBE_BIN};
};

TEST_F(SyscallFilterTest, ACallTheTablesAllowRunsToTheEnd) {
    const auto child = run({kProbe.string(), "getpid"});
    EXPECT_EQ(child.exit_code, 0) << child.stderr_tail;
    EXPECT_EQ(child.signal, 0);
}

TEST_F(SyscallFilterTest, EveryOtherCallKillsTheProgramWithSigsysAndClassifiesAsBlocked) {
    for (const std::string name : {"ptrace", "mount", "keyctl", "bpf", "io_uring_setup", "socket",
                                   "unshare", "setns", "kill", "process_vm_readv", "chroot"}) {
        const auto child = run({kProbe.string(), name});
        EXPECT_EQ(child.signal, SIGSYS) << name;
        EXPECT_EQ(infra::ffmpeg::classify(child.exit_code, child.signal, child.ending),
                  core::ports::TranscodeFailure::SyscallBlocked)
            << name;
    }
}

TEST_F(SyscallFilterTest, AnAbortStillEndsTheProgramWithSigabrtAndIsAPlainKill) {
    const auto child = run({kProbe.string(), "abort"});
    EXPECT_EQ(child.signal, SIGABRT);
    EXPECT_EQ(infra::ffmpeg::classify(child.exit_code, child.signal, child.ending),
              core::ports::TranscodeFailure::Killed);
}

TEST_F(SyscallFilterTest, WithoutTheFilterTheSameCallsReturnAnError) {
    syscall_filter_ = false;
    for (const std::string name : {"ptrace", "mount", "keyctl", "bpf", "io_uring_setup"}) {
        const auto child = run({kProbe.string(), name});
        EXPECT_EQ(child.signal, 0) << name;
        EXPECT_EQ(child.exit_code, 0) << name;
    }
}

TEST_F(SyscallFilterTest, ATranscodeWithThreadsStillRuns) {
    Limits limits;
    limits.address_space_bytes = 8 * kGiB;
    const auto child = run({"ffmpeg",
                            "-nostdin",
                            "-v",
                            "error",
                            "-f",
                            "lavfi",
                            "-i",
                            "testsrc2=size=640x360:rate=25",
                            "-f",
                            "lavfi",
                            "-i",
                            "sine=frequency=440:sample_rate=48000",
                            "-t",
                            "2",
                            "-c:v",
                            "libx264",
                            "-threads",
                            "4",
                            "-c:a",
                            "aac",
                            "-f",
                            "hls",
                            "-hls_time",
                            "1",
                            "-hls_segment_type",
                            "fmp4",
                            "-hls_playlist_type",
                            "vod",
                            "-hls_segment_filename",
                            "seg_%03d.m4s",
                            "index.m3u8"},
                           limits);
    if (child.exit_code == infra::ffmpeg::kProgramNotFound) {
        GTEST_SKIP() << "no ffmpeg on this host";
    }
    EXPECT_EQ(child.exit_code, 0) << child.stderr_tail;
    EXPECT_TRUE(fs::exists(writable_.path() / "index.m3u8"));
    EXPECT_TRUE(fs::exists(writable_.path() / "seg_000.m4s"));
}

// The live packager's command line, fed MPEG-TS over a pipe as the packager feeds it. It renames
// each segment into place, which the worker's transcode never does.
TEST_F(SyscallFilterTest, ALiveRemuxFromAPipeStillRuns) {
    // The source is an encode, which the packager never runs: x264 at its default thread count
    // peaked at 0.95 GiB of address space under ffmpeg 6.1 and passes 1 GiB under 7.1 (the
    // worker image's, docs/adr/0074). The remux below keeps the default 1 GiB, the packager's.
    Limits encode;
    encode.address_space_bytes = 4 * kGiB;
    const auto source =
        run({"ffmpeg", "-nostdin", "-v",       "error",
             "-f",     "lavfi",    "-i",       "testsrc2=size=320x180:rate=25",
             "-f",     "lavfi",    "-i",       "sine=frequency=440:sample_rate=48000",
             "-t",     "3",        "-c:v",     "libx264",
             "-g",     "25",       "-c:a",     "aac",
             "-f",     "mpegts",   "source.ts"},
            encode);
    if (source.exit_code == infra::ffmpeg::kProgramNotFound) {
        GTEST_SKIP() << "no ffmpeg on this host";
    }
    ASSERT_EQ(source.exit_code, 0) << source.stderr_tail;
    std::string bytes(fs::file_size(writable_.path() / "source.ts"), '\0');
    ASSERT_FALSE(bytes.empty());
    std::ifstream(writable_.path() / "source.ts", std::ios::binary)
        .read(bytes.data(), static_cast<std::streamsize>(bytes.size()));

    std::array<int, 2> fds{};
    ASSERT_EQ(::pipe2(fds.data(), O_CLOEXEC), 0);
    os::UniqueFd read_end(fds[0]);
    // More than a pipe holds, so a writer of its own; closing its end is the end of the stream.
    std::jthread publisher([write_end = os::UniqueFd(fds[1]), &bytes] {
        // A write with no reader left is EPIPE here rather than SIGPIPE for the whole test.
        sigset_t pipe_signal;
        sigemptyset(&pipe_signal);
        sigaddset(&pipe_signal, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &pipe_signal, nullptr);
        std::string_view rest = bytes;
        while (!rest.empty()) {
            const ssize_t n = ::write(write_end.get(), rest.data(), rest.size());
            if (n <= 0) {
                return;
            }
            rest.remove_prefix(static_cast<std::size_t>(n));
        }
    });
    const infra::ffmpeg::LiveRemuxJob job{.input = read_end.get(),
                                          .out_dir = writable_.path(),
                                          .segment_seconds = 1,
                                          .listed_segments = 2,
                                          .first_sequence = 5,
                                          .epoch = 7,
                                          .max_kbps = 8000,
                                          .max_duration = core::Seconds{30}};
    const auto child =
        run(infra::ffmpeg::live_remux_args(
                "ffmpeg", job, infra::ffmpeg::live_probe(job.max_kbps, job.segment_seconds)),
            {}, {}, read_end.get());
    // An ffmpeg that died early leaves the writer blocked on a full pipe until no reader is left.
    read_end.reset();
    publisher.join();
    EXPECT_EQ(child.signal, 0) << "SIGSYS is the filter killing it";
    EXPECT_EQ(child.exit_code, 0) << child.stderr_tail;
    const std::string segment = infra::ffmpeg::live_segment_name(7, 5);
    EXPECT_TRUE(fs::exists(writable_.path() / infra::ffmpeg::live_init_name(7)));
    EXPECT_TRUE(fs::exists(writable_.path() / segment));
    EXPECT_FALSE(fs::exists(writable_.path() / (segment + ".tmp")));
    EXPECT_TRUE(fs::exists(writable_.path() / std::string(infra::ffmpeg::kLivePlaylist)));
}

// The traces this filter came from were taken as root, and some libraries ask more of a caller
// that is not (libgcrypt calls geteuid only then). CI runs these tests as an ordinary user;
// as root this stands in for it.
TEST_F(SyscallFilterTest, AnOrdinaryUsersFfprobeRunsToo) {
    if (::geteuid() != 0) {
        GTEST_SKIP() << "already an ordinary user, as every test here then is";
    }
    constexpr uid_t kNobody = 65534;
    fs::permissions(writable_.path(), fs::perms::all);
    const pid_t child = ::fork();
    if (child == 0) {
        const bool dropped =
            ::setgroups(0, nullptr) == 0 && ::setgid(kNobody) == 0 && ::setuid(kNobody) == 0;
        const auto ran =
            dropped ? run({"ffprobe", "-v", "error", "-f", "lavfi", "-i",
                           "testsrc=duration=1:size=64x64", "-show_entries", "format=duration"})
                    : ChildExit{.exit_code = 99,
                                .signal = 0,
                                .ending = Ending::Exited,
                                .wall = {},
                                .peak_rss_kib = 0,
                                .stderr_tail = {},
                                .stderr_bytes = 0};
        std::_Exit(ran.exit_code);
    }
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    if (WEXITSTATUS(status) == infra::ffmpeg::kProgramNotFound) {
        GTEST_SKIP() << "no ffprobe on this host";
    }
    EXPECT_EQ(WEXITSTATUS(status), 0) << "159 is the filter killing it";
}

TEST(SandboxCheck, NamesWhatWentWrong) {
    const os::SystemClock clock;
    const auto refused = infra::ffmpeg::check_sandbox("/nonexistent/ulw_sandbox", "/tmp", clock);
    ASSERT_TRUE(refused);
    EXPECT_NE(refused->find("/nonexistent/ulw_sandbox"), std::string::npos);
}

} // namespace
