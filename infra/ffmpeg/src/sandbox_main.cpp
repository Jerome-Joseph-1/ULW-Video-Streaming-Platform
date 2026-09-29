// ulw_sandbox --writable DIR --address-space BYTES --cpu-seconds N [--file-size BYTES]
//             [--no-syscall-filter] -- PROGRAM [ARGS...]
//
// Runs PROGRAM confined: an empty network namespace (unshare -n), a pid namespace with a
// /proc of its own, every mount read-only except DIR, the given RLIMIT_AS and RLIMIT_CPU, and
// when asked for RLIMIT_FSIZE, the size a file may grow to (SIGXFSZ ends a program at it), no
// core dumps, no descriptors beyond the standard three, no capabilities, so the program
// cannot undo any of it, and last a seccomp filter that kills it for any system call ffmpeg
// has no use for (seccomp_filter.hpp). --no-syscall-filter leaves that one out, for tests that
// need an ordinary shell as the program. posix_spawn cannot do any of this in the child, hence a
// separate program. Without a PROGRAM it sets everything up and exits 0, which is how the worker
// checks at startup that the host allows it.
//
// Three processes: this helper, which the worker started; its child, pid 1 of the new pid
// namespace; and PROGRAM, pid 1's child. When pid 1 exits the kernel kills everything left in
// the namespace, so nothing PROGRAM started, not even a descendant that left its session,
// outlives it or keeps its pipes open. pid 1 dies with the helper, and the helper with the
// worker. SIGTERM and SIGINT sent to the helper are passed down to PROGRAM, and the helper
// ends as PROGRAM did: with its exit code, or killed by the same signal. The signal is raised
// again rather than folded into 128 + its number, because ffmpeg's own failures exit with
// 256 minus an error code (183 for invalid data) and would read as signals.
//
// Exit codes of its own: 125 when a confinement step fails, 126 when PROGRAM cannot be
// executed, 127 when it is not found.
#include "core/util/parse.hpp"

#include "seccomp_filter.hpp"

#include <linux/capability.h>
#include <linux/securebits.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <optional>
#include <poll.h>
#include <print>
#include <pthread.h>
#include <sched.h>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kSetupFailed = 125;
constexpr int kCannotExecute = 126;
constexpr int kNotFound = 127;

struct Options {
    std::filesystem::path writable;
    rlim_t address_space = 0;
    rlim_t cpu_seconds = 0;
    // 0: no limit on the size of a file the program writes.
    rlim_t file_size = 0;
    bool syscall_filter = true;
    std::vector<char*> program;
};

[[noreturn]] void die(std::string_view step, int error) {
    std::println(stderr, "ulw_sandbox: {}: {}", step, std::generic_category().message(error));
    std::_Exit(kSetupFailed);
}

[[noreturn]] void usage() {
    std::println(stderr, "usage: ulw_sandbox --writable DIR --address-space BYTES "
                         "--cpu-seconds N [--file-size BYTES] [--no-syscall-filter] -- "
                         "[PROGRAM ARGS...]");
    std::_Exit(kSetupFailed);
}

Options parse(std::span<char*> args) {
    Options options;
    std::size_t i = 1;
    for (; i < args.size(); ++i) {
        const std::string_view flag = args[i];
        if (flag == "--") {
            ++i;
            break;
        }
        if (flag == "--no-syscall-filter") {
            options.syscall_filter = false;
            continue;
        }
        if (i + 1 == args.size()) {
            usage();
        }
        const std::string_view value = args[++i];
        if (flag == "--writable") {
            options.writable = value;
        } else if (flag == "--address-space") {
            options.address_space = core::parse_integer<rlim_t>(value).value_or(0);
        } else if (flag == "--cpu-seconds") {
            options.cpu_seconds = core::parse_integer<rlim_t>(value).value_or(0);
        } else if (flag == "--file-size") {
            options.file_size = core::parse_integer<rlim_t>(value).value_or(0);
        } else {
            usage();
        }
    }
    if (options.writable.empty() || options.address_space == 0 || options.cpu_seconds == 0) {
        usage();
    }
    for (; i < args.size(); ++i) {
        options.program.push_back(args[i]);
    }
    options.program.push_back(nullptr);
    return options;
}

bool write_file(const char* path, std::string_view text) {
    const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    const bool ok = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    ::close(fd);
    return ok;
}

// As root (or with CAP_SYS_ADMIN) the namespaces come directly. Otherwise a user namespace
// grants the capability inside it; our own ids map to themselves, so files keep their owners.
// The new pid namespace is our children's, not ours.
void enter_namespaces() {
    constexpr int kNamespaces = CLONE_NEWNET | CLONE_NEWNS | CLONE_NEWPID;
    if (::unshare(kNamespaces) == 0) {
        return;
    }
    if (errno != EPERM) {
        die("unshare", errno);
    }
    const uid_t uid = ::geteuid();
    const gid_t gid = ::getegid();
    if (::unshare(CLONE_NEWUSER | kNamespaces) != 0) {
        die("unshare with a user namespace", errno);
    }
    // The kernel refuses a gid_map from an unprivileged writer until setgroups is denied.
    if (!write_file("/proc/self/setgroups", "deny") ||
        !write_file("/proc/self/uid_map", std::format("{} {} 1", uid, uid)) ||
        !write_file("/proc/self/gid_map", std::format("{} {} 1", gid, gid))) {
        die("id maps", errno);
    }
}

void confine_filesystem(const std::filesystem::path& writable) {
    // Nothing done here may leak back into the parent's mount namespace.
    if (::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
        die("make mounts private", errno);
    }
    // The host's /proc lists every process there, the worker included; this one lists the
    // namespace's own.
    if (::mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr) != 0) {
        die("mount /proc", errno);
    }
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::canonical(writable, ec);
    if (ec) {
        die("writable directory", ec.value());
    }
    // A mount of its own, so it can be made writable again after everything else is not.
    if (::mount(dir.c_str(), dir.c_str(), nullptr, MS_BIND | MS_REC, nullptr) != 0) {
        die("bind the writable directory", errno);
    }
    mount_attr read_only{};
    read_only.attr_set = MOUNT_ATTR_RDONLY;
    if (::mount_setattr(AT_FDCWD, "/", AT_RECURSIVE, &read_only, sizeof read_only) != 0) {
        die("make mounts read-only", errno);
    }
    mount_attr writable_again{};
    writable_again.attr_clr = MOUNT_ATTR_RDONLY;
    if (::mount_setattr(AT_FDCWD, dir.c_str(), 0, &writable_again, sizeof writable_again) != 0) {
        die("make the writable directory writable", errno);
    }
    if (::chdir(dir.c_str()) != 0) {
        die("chdir", errno);
    }
}

// Mounts made read-only here can be made writable again by anyone holding CAP_SYS_ADMIN in
// this mount namespace, and we are root in it. Every capability goes, for good: the bounding
// set, the sets we hold, and root's right to regain them at execve.
void drop_capabilities() {
    constexpr unsigned long kNoRootEver = SECBIT_NOROOT | SECBIT_NOROOT_LOCKED |
                                          SECBIT_NO_SETUID_FIXUP | SECBIT_NO_SETUID_FIXUP_LOCKED |
                                          SECBIT_KEEP_CAPS_LOCKED;
    if (::prctl(PR_SET_SECUREBITS, kNoRootEver) != 0) {
        die("securebits", errno);
    }
    // Capabilities are numbered below 64; numbers this kernel does not know fail with EINVAL.
    constexpr unsigned long kCapabilitySlots = 64;
    for (unsigned long cap = 0; cap < kCapabilitySlots; ++cap) {
        if (::prctl(PR_CAPBSET_DROP, cap) != 0 && errno != EINVAL) {
            die("drop the bounding set", errno);
        }
    }
    __user_cap_header_struct header{.version = _LINUX_CAPABILITY_VERSION_3, .pid = 0};
    std::array<__user_cap_data_struct, _LINUX_CAPABILITY_U32S_3> none{};
    if (::syscall(SYS_capset, &header, none.data()) != 0) {
        die("capset", errno);
    }
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        die("no new privileges", errno);
    }
}

void set_limit(int resource, rlim_t soft, rlim_t hard, std::string_view name) {
    const rlimit limit{.rlim_cur = soft, .rlim_max = hard};
    if (::setrlimit(resource, &limit) != 0) {
        die(name, errno);
    }
}

// 128 + the signal for a signalled process, as a shell reports it.
int exit_code_of(int status) noexcept {
    constexpr int kSignalled = 128;
    return WIFEXITED(status) ? WEXITSTATUS(status) : kSignalled + WTERMSIG(status);
}

// Waits for `child` to exit, passing SIGTERM and SIGINT on to it, and reaps every other
// process that ends up as ours on the way: pid 1 inherits the program's orphans.
// `signals` must be blocked, so that they queue until taken here. Returns the wait status.
int wait_passing_signals(pid_t child, const sigset_t& signals) {
    while (true) {
        const int sig = ::sigwaitinfo(&signals, nullptr);
        if (sig == SIGTERM || sig == SIGINT) {
            static_cast<void>(::kill(child, sig));
            continue;
        }
        int status = 0;
        pid_t reaped = 0;
        while ((reaped = ::waitpid(-1, &status, WNOHANG)) > 0) {
            if (reaped == child) {
                return status;
            }
        }
    }
}

// pid 1 of the new pid namespace: confines itself, then starts the program as its child.
// `status_out` carries PROGRAM's wait status to the helper, which cannot wait for it itself.
[[noreturn]] void run_init(const Options& options, int lifeline, int status_out,
                           const sigset_t& signals) {
    // The helper may die without killing us, and then the namespace must go too. getppid()
    // is 0 here, the helper being outside the namespace, so the lifeline tells instead: its
    // other end closes when the helper exits.
    if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) {
        die("parent death signal", errno);
    }
    pollfd helper_gone{.fd = lifeline, .events = 0, .revents = 0};
    if (::poll(&helper_gone, 1, 0) != 0) {
        std::_Exit(kSetupFailed);
    }
    confine_filesystem(options.writable);
    set_limit(RLIMIT_AS, options.address_space, options.address_space, "RLIMIT_AS");
    // SIGXCPU at the soft limit; the hard limit one second later is SIGKILL for a child that
    // catches SIGXCPU.
    set_limit(RLIMIT_CPU, options.cpu_seconds, options.cpu_seconds + 1, "RLIMIT_CPU");
    if (options.file_size != 0) {
        set_limit(RLIMIT_FSIZE, options.file_size, options.file_size, "RLIMIT_FSIZE");
    }
    // A crash dump of a hostile input is hostile data in the scratch directory we upload from.
    set_limit(RLIMIT_CORE, 0, 0, "RLIMIT_CORE");
    // All but `status_out`, which is close-on-exec, so PROGRAM never holds it.
    const auto keep = static_cast<unsigned>(status_out);
    if ((keep > 3 && ::close_range(3, keep - 1, 0) != 0) || ::close_range(keep + 1, ~0U, 0) != 0) {
        die("close descriptors", errno);
    }
    drop_capabilities();
    // Even with nothing to run, the child is started and filtered, which is how the worker
    // learns at startup that the host lets it install the filter.
    // This helper runs no threads, so the forked child may do anything.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const pid_t program = ::fork();
    if (program < 0) {
        die("fork the program", errno);
    }
    if (program == 0) {
        sigset_t none;
        sigemptyset(&none);
        ::pthread_sigmask(SIG_SETMASK, &none, nullptr);
        // After everything else, which the filter would not let the child do, and not in pid 1,
        // which forks the program.
        if (options.syscall_filter && !infra::ffmpeg::seccomp::install()) {
            die("syscall filter", errno);
        }
        if (options.program.front() == nullptr) {
            std::_Exit(EXIT_SUCCESS);
        }
        ::execvp(options.program.front(), options.program.data());
        const int error = errno;
        std::println(stderr, "ulw_sandbox: exec {}: {}", options.program.front(),
                     std::generic_category().message(error));
        std::_Exit(error == ENOENT ? kNotFound : kCannotExecute);
    }
    const int status = wait_passing_signals(program, signals);
    // A pipe write this small is atomic; the helper reads it once we have exited.
    [[maybe_unused]] const ssize_t n = ::write(status_out, &status, sizeof status);
    std::_Exit(exit_code_of(status));
}

// Ends this process the way `status` says its child ended. pid 1 of a namespace cannot do
// this, the kernel dropping the signals it sends itself, which is why the helper does.
int end_like(int status) {
    if (!WIFSIGNALED(status)) {
        return WEXITSTATUS(status);
    }
    const int sig = WTERMSIG(status);
    // A dump of this helper would be as useless as the program's is dangerous.
    const rlimit no_core{.rlim_cur = 0, .rlim_max = 0};
    static_cast<void>(::setrlimit(RLIMIT_CORE, &no_core));
    static_cast<void>(::signal(sig, SIG_DFL));
    sigset_t only;
    sigemptyset(&only);
    sigaddset(&only, sig);
    static_cast<void>(::pthread_sigmask(SIG_UNBLOCK, &only, nullptr));
    static_cast<void>(::raise(sig));
    // Only a signal whose default is not to terminate gets here, and none of those ended
    // the program.
    return exit_code_of(status);
}

int run(std::span<char*> args) {
    const Options options = parse(args);

    // The worker may die without killing us; nothing must outlive it. The check after the
    // prctl catches a worker that died before it.
    const pid_t parent = ::getppid();
    if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || ::getppid() != parent) {
        die("parent death signal", errno);
    }
    enter_namespaces();

    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGCHLD);
    if (const int rc = ::pthread_sigmask(SIG_BLOCK, &signals, nullptr); rc != 0) {
        die("block signals", rc);
    }
    std::array<int, 2> lifeline{};
    if (::pipe2(lifeline.data(), O_CLOEXEC) != 0) {
        die("lifeline", errno);
    }
    std::array<int, 2> program_status{};
    if (::pipe2(program_status.data(), O_CLOEXEC) != 0) {
        die("status pipe", errno);
    }
    // NOLINTNEXTLINE(concurrency-mt-unsafe): no threads here, as above.
    const pid_t init = ::fork();
    if (init < 0) {
        die("fork into the pid namespace", errno);
    }
    if (init == 0) {
        ::close(lifeline[1]);
        ::close(program_status[0]);
        run_init(options, lifeline[0], program_status[1], signals);
    }
    // Our end of the lifeline stays open until we exit.
    ::close(lifeline[0]);
    ::close(program_status[1]);
    const int init_status = wait_passing_signals(init, signals);
    int status = 0;
    // pid 1 wrote before it exited, or never will: a pid 1 that failed to start the program
    // or was killed leaves the pipe empty, and its own status stands.
    if (::read(program_status[0], &status, sizeof status) == sizeof status) {
        return end_like(status);
    }
    return end_like(init_status);
}

} // namespace

// Formatting and allocation are all that can throw; either is a setup failure.
int main(int argc, char** argv) {
    try {
        return run(std::span(argv, static_cast<std::size_t>(argc)));
    } catch (...) {
        return kSetupFailed;
    }
}
