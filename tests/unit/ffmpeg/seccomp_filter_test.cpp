// The filter program itself, run by a small interpreter of the classic BPF it uses, so that
// every verdict is checked on any host. What the kernel does with it is in sandbox_test.cpp.
#include "seccomp_filter.hpp"

#include <sys/resource.h>

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <optional>
#include <vector>

namespace {

namespace seccomp = infra::ffmpeg::seccomp;

// Runs `program` over `data`; nullopt if it does anything the interpreter does not know or
// leaves the program, which the kernel would refuse to load.
std::optional<std::uint32_t> run(const std::vector<sock_filter>& program,
                                 const seccomp_data& data) {
    std::uint32_t a = 0;
    std::array<std::byte, sizeof data> raw{};
    std::memcpy(raw.data(), &data, sizeof data);
    std::size_t pc = 0;
    // Filters have no backward jumps, so no run is longer than the program.
    for (std::size_t steps = 0; steps <= program.size() && pc < program.size(); ++steps) {
        const sock_filter& insn = program[pc++];
        switch (insn.code) {
        case BPF_LD | BPF_W | BPF_ABS:
            if (insn.k + sizeof a > raw.size()) {
                return std::nullopt;
            }
            std::memcpy(&a, raw.data() + insn.k, sizeof a);
            break;
        case BPF_ALU | BPF_AND | BPF_K:
            a &= insn.k;
            break;
        case BPF_JMP | BPF_JEQ | BPF_K:
            pc += a == insn.k ? insn.jt : insn.jf;
            break;
        case BPF_JMP | BPF_JA:
            pc += insn.k;
            break;
        case BPF_RET | BPF_K:
            return insn.k;
        default:
            return std::nullopt;
        }
    }
    return std::nullopt;
}

seccomp_data call(long nr, std::uint64_t a0 = 0, std::uint64_t a1 = 0, std::uint64_t a2 = 0) {
    seccomp_data data{};
    data.nr = static_cast<int>(nr);
    data.arch = seccomp::kAudit;
    data.args[0] = a0;
    data.args[1] = a1;
    data.args[2] = a2;
    return data;
}

class SeccompFilter : public ::testing::Test {
protected:
    [[nodiscard]] std::optional<std::uint32_t> verdict(const seccomp_data& data) const {
        return run(program_, data);
    }

    const std::vector<sock_filter> program_ = seccomp::build_program();
};

constexpr std::uint64_t kThread = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
                                  CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID |
                                  CLONE_CHILD_CLEARTID;

TEST_F(SeccompFilter, FitsWhatTheKernelWillLoad) {
    EXPECT_LE(program_.size(), std::size_t{BPF_MAXINSNS});
}

// Every compiler and library must build the program instruction for instruction the same. A
// change to the allowlist changes this too, deliberately: the new value is the reviewer's cue to
// look at what was added. FNV-1a over each instruction's four fields.
TEST_F(SeccompFilter, ProgramIsTheSameInstructionsOnEveryCompiler) {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto mix = [&hash](std::uint64_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            hash ^= (value >> (8 * i)) & 0xFFU;
            hash *= 1099511628211ULL;
        }
    };
    for (const sock_filter& insn : program_) {
        mix(insn.code, 2);
        mix(insn.jt, 1);
        mix(insn.jf, 1);
        mix(insn.k, 4);
    }
#if defined(__x86_64__)
    EXPECT_EQ(program_.size(), 168U);
    EXPECT_EQ(hash, 4688286380803353990ULL) << "program of " << program_.size() << " instructions";
#else
    GTEST_SKIP() << "the golden program is x86-64's";
#endif
}

TEST_F(SeccompFilter, AllowsEveryCallTheTracesShowed) {
    for (const int nr : seccomp::kAllowed) {
        EXPECT_EQ(verdict(call(nr)), seccomp::kAllow) << "syscall " << nr;
    }
}

TEST_F(SeccompFilter, KillsEveryNumberOutsideItsTables) {
    const auto known = [](long nr) {
        return std::ranges::contains(seccomp::kAllowed, static_cast<int>(nr)) || nr == SYS_ioctl ||
               nr == SYS_prctl || nr == SYS_fcntl || nr == SYS_prlimit64 || nr == SYS_clone ||
               nr == SYS_tgkill || nr == SYS_tkill || nr == SYS_clone3;
    };
    // Well past the highest number any kernel assigns, and the x32 range on top of it.
    for (long nr = 0; nr < 1024; ++nr) {
        if (!known(nr)) {
            EXPECT_EQ(verdict(call(nr)), seccomp::kKill) << "syscall " << nr;
        }
    }
    for (long nr = 0; nr < 1024; ++nr) {
        EXPECT_EQ(verdict(call(nr | 0x40000000L, 0, 0, 0)), seccomp::kKill) << "x32 syscall " << nr;
    }
}

TEST_F(SeccompFilter, KillsTheCallsAnExploitReachesFor) {
    for (const long nr : {SYS_ptrace,
                          SYS_mount,
                          SYS_umount2,
                          SYS_keyctl,
                          SYS_add_key,
                          SYS_bpf,
                          SYS_io_uring_setup,
                          SYS_io_uring_enter,
                          SYS_socket,
                          SYS_connect,
                          SYS_unshare,
                          SYS_setns,
                          SYS_kill,
                          SYS_tgkill,
                          SYS_execveat,
                          SYS_chroot,
                          SYS_pivot_root,
                          SYS_init_module,
                          SYS_finit_module,
                          SYS_perf_event_open,
                          SYS_userfaultfd,
                          SYS_process_vm_readv,
                          SYS_process_vm_writev,
                          SYS_open_by_handle_at,
                          SYS_seccomp,
                          SYS_setuid,
                          SYS_capset,
                          SYS_reboot,
                          SYS_memfd_create,
                          SYS_mknod,
                          SYS_symlink,
                          SYS_setsid}) {
        EXPECT_EQ(verdict(call(nr)), seccomp::kKill) << "syscall " << nr;
    }
    // Processes are made with fork and vfork, which x86-64 has and other architectures do not.
#if defined(SYS_fork)
    EXPECT_EQ(verdict(call(SYS_fork)), seccomp::kKill);
    EXPECT_EQ(verdict(call(SYS_vfork)), seccomp::kKill);
#endif
}

TEST_F(SeccompFilter, KillsACallFromAnotherArchitecture) {
    seccomp_data data = call(SYS_read);
    data.arch = AUDIT_ARCH_I386;
    EXPECT_EQ(verdict(data), seccomp::kKill);
}

TEST_F(SeccompFilter, AllowsATerminalQueryButNotOtherIoctls) {
    EXPECT_EQ(verdict(call(SYS_ioctl, 2, 0x5401)), seccomp::kAllow);
    // TIOCSTI: pushes a byte into a terminal's input.
    EXPECT_EQ(verdict(call(SYS_ioctl, 2, 0x5412)), seccomp::kKill);
    EXPECT_EQ(verdict(call(SYS_ioctl, 2, 0)), seccomp::kKill);
}

TEST_F(SeccompFilter, AllowsNamingAThreadButNotOtherPrctls) {
    EXPECT_EQ(verdict(call(SYS_prctl, PR_SET_NAME)), seccomp::kAllow);
    EXPECT_EQ(verdict(call(SYS_prctl, PR_CAPBSET_READ)), seccomp::kAllow);
    EXPECT_EQ(verdict(call(SYS_prctl, PR_SET_SECCOMP)), seccomp::kKill);
    EXPECT_EQ(verdict(call(SYS_prctl, PR_SET_DUMPABLE)), seccomp::kKill);
    EXPECT_EQ(verdict(call(SYS_prctl, PR_SET_MM)), seccomp::kKill);
}

TEST_F(SeccompFilter, AllowsDescriptorFlagsButNotLeasesOrSignals) {
    EXPECT_EQ(verdict(call(SYS_fcntl, 3, F_SETFD)), seccomp::kAllow);
    EXPECT_EQ(verdict(call(SYS_fcntl, 3, F_GETFL)), seccomp::kAllow);
    EXPECT_EQ(verdict(call(SYS_fcntl, 3, F_DUPFD_CLOEXEC)), seccomp::kAllow);
    EXPECT_EQ(verdict(call(SYS_fcntl, 3, F_SETLEASE)), seccomp::kKill);
    EXPECT_EQ(verdict(call(SYS_fcntl, 3, F_SETOWN)), seccomp::kKill);
}

TEST_F(SeccompFilter, AllowsAnAbortButNoOtherSignalToBeSent) {
    EXPECT_EQ(verdict(call(SYS_tgkill, 1, 2, SIGABRT)), seccomp::kAllow);
    EXPECT_EQ(verdict(call(SYS_tkill, 2, SIGABRT)), seccomp::kAllow);
    for (const int sig : {SIGKILL, SIGTERM, SIGSEGV, SIGUSR1, 0}) {
        EXPECT_EQ(verdict(call(SYS_tgkill, 1, 2, static_cast<std::uint64_t>(sig))), seccomp::kKill)
            << sig;
        EXPECT_EQ(verdict(call(SYS_tkill, 2, static_cast<std::uint64_t>(sig))), seccomp::kKill)
            << sig;
    }
}

TEST_F(SeccompFilter, AllowsReadingOwnLimitsButNotSettingAny) {
    EXPECT_EQ(verdict(call(SYS_prlimit64, 0, RLIMIT_STACK, 0)), seccomp::kAllow);
    EXPECT_EQ(verdict(call(SYS_prlimit64, 0, RLIMIT_AS, 0x7ffc0000)), seccomp::kKill);
    // Another process's limits, whatever the pointer.
    EXPECT_EQ(verdict(call(SYS_prlimit64, 1, RLIMIT_AS, 0)), seccomp::kKill);
    // A pointer whose low word alone is zero is not null.
    EXPECT_EQ(verdict(call(SYS_prlimit64, 0, RLIMIT_AS, std::uint64_t{1} << 32U)), seccomp::kKill);
}

TEST_F(SeccompFilter, AllowsAThreadButNotAProcessOrANamespace) {
    EXPECT_EQ(verdict(call(SYS_clone, kThread)), seccomp::kAllow);
    // What fork does.
    EXPECT_EQ(verdict(call(SYS_clone, SIGCHLD)), seccomp::kKill);
    EXPECT_EQ(verdict(call(SYS_clone, CLONE_VM | CLONE_VFORK | SIGCHLD)), seccomp::kKill);
    // A thread that also asks for a namespace of its own.
    for (const std::uint64_t ns :
         {std::uint64_t{CLONE_NEWUSER}, std::uint64_t{CLONE_NEWNET}, std::uint64_t{CLONE_NEWNS},
          std::uint64_t{CLONE_NEWPID}, std::uint64_t{CLONE_NEWIPC}, std::uint64_t{CLONE_NEWUTS},
          std::uint64_t{CLONE_NEWCGROUP}, std::uint64_t{0x80}}) {
        EXPECT_EQ(verdict(call(SYS_clone, kThread | ns)), seccomp::kKill) << ns;
    }
}

TEST_F(SeccompFilter, AnswersTheNewerCloneWithNoSuchCallSoThatTheOlderIsUsed) {
    EXPECT_EQ(verdict(call(SYS_clone3, kThread)), seccomp::kNoSys);
    EXPECT_EQ(seccomp::kNoSys & SECCOMP_RET_ACTION_FULL, SECCOMP_RET_ERRNO);
    EXPECT_EQ(seccomp::kNoSys & SECCOMP_RET_DATA, static_cast<std::uint32_t>(ENOSYS));
}

TEST_F(SeccompFilter, EveryVerdictIsOneOfThreeWhateverTheArguments) {
    // Arguments that are all ones stand for the worst a caller can put in a register.
    for (long nr = 0; nr < 512; ++nr) {
        const auto v = verdict(call(nr, ~0ULL, ~0ULL, ~0ULL));
        ASSERT_TRUE(v) << nr;
        EXPECT_TRUE(*v == seccomp::kAllow || *v == seccomp::kKill || *v == seccomp::kNoSys) << nr;
    }
}

} // namespace
