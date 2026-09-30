// The syscall filter ulw_sandbox installs in the program it starts, as classic BPF written out
// here rather than through libseccomp, which the images do not carry and the helper would have
// to link. Header-only for the same reason as sandbox_main.cpp: the helper links nothing that
// is instrumented, and the tests read the same program the helper installs.
//
// The allowlist is what ffmpeg 6.1.1 and ffprobe called in strace -f runs: the probe and the
// three-rung HLS transcode the worker issues, over h264/aac mp4, vp9/opus webm, mpeg4/mp3 avi,
// hevc/aac and av1 mkv, mpegts, flv, ogg, wmv and prores mov inputs, and over truncated,
// random and empty files. Every input gave the same 40 names as root (41 as an ordinary user), the
// runtime's start-up and thread creation among them, and the live packager's remux one more; ioctl,
// prctl, fcntl and prlimit64 are admitted only with the arguments below, and clone3, which glibc
// used for threads, is answered ENOSYS so that it uses clone, also checked. The additions marked
// below are calls the traces could not reach because they need a signal or a clock the runs never
// met, and tgkill and tkill for SIGABRT alone. Anything else kills the whole process: ptrace,
// mount, keyctl, bpf, io_uring_setup, socket, unshare, setns, fork, kill and the rest of the
// kernel's surface a decoder exploit would reach for.
//
// Re-traced for the worker image's move to ffmpeg 7.1.5 on Debian 13, glibc 2.41 (docs/adr/0074),
// with tools/trace-ffmpeg-syscalls.sh, which also runs the worker's output checks and the live
// recording's remux: 39 names as root and as an ordinary user, every one of them already here,
// so nothing was added. The ffmpeg suites and the worker's, the live packager's and the live
// recording's integration suites passed under this filter on that image's ffmpeg.
#pragma once

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/sched.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <sched.h>
#include <span>
#include <unistd.h>
#include <utility>
#include <vector>

namespace infra::ffmpeg::seccomp {

static_assert(std::endian::native == std::endian::little,
              "the argument offsets below assume the low word comes first");

#if defined(__x86_64__)
inline constexpr std::uint32_t kAudit = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
inline constexpr std::uint32_t kAudit = AUDIT_ARCH_AARCH64;
#else
#error "the syscall filter has no table for this architecture"
#endif

// Most frequent first: a call is compared against each entry before it, and the kernel's
// bitmap cache only helps entries whose verdict never depends on arguments.
inline constexpr std::array kAllowed = {
    SYS_futex,
    SYS_mmap,
    SYS_mprotect,
    SYS_read,
    SYS_openat,
    SYS_fstat,
    SYS_close,
    SYS_rt_sigprocmask,
    SYS_munmap,
    SYS_madvise,
    SYS_brk,
    SYS_rseq,
    SYS_set_robust_list,
    SYS_exit,
    SYS_write,
    SYS_lseek,
    SYS_mremap,
    SYS_newfstatat,
    SYS_sched_getaffinity,
    SYS_rt_sigaction,
    SYS_statfs,
    SYS_pread64,
    SYS_getrandom,
    SYS_getdents64,
    SYS_set_tid_address,
    SYS_mlock,
    SYS_getuid,
    SYS_getpid,
    SYS_exit_group,
    SYS_getrusage,
    SYS_dup,
    // Once, by the helper's own child: the program itself. What it execs inherits this filter.
    SYS_execve,
#if defined(SYS_access)
    SYS_access,
    SYS_mkdir,
#else
    SYS_faccessat,
    SYS_mkdirat,
#endif
#if defined(SYS_arch_prctl)
    SYS_arch_prctl,
#endif
    // Found by running as an ordinary user, which the root traces could not show: libgcrypt's
    // secure-memory set-up calls geteuid after getuid and mlock, but only when not root. The
    // first run as CI's runner (a pod's uid 10001 does the same) died of it.
    SYS_geteuid,
    // Not in any trace, for want of the occasion. A signal handler, as ffmpeg installs for
    // SIGTERM and SIGINT, returns through rt_sigreturn, and the worker's stop request is a
    // SIGTERM. The clocks and sleeps are the vDSO's fallbacks and the waits in libav* and
    // x264's threads; gettid is the C library's own.
    SYS_rt_sigreturn,
    SYS_clock_gettime,
    SYS_gettimeofday,
    SYS_clock_nanosleep,
    SYS_nanosleep,
    SYS_sched_yield,
    SYS_gettid,
#if defined(SYS_rename)
    // The live packager's remux sets hls_flags temp_file (live_command.cpp): the HLS muxer's
    // thread writes every segment and the playlist under a .tmp name and renames it once closed,
    // so the first finished segment died of this. The worker's VOD output is written in place.
    // glibc's rename() is this call where the architecture has it and renameat where it does
    // not. Both names stay in the writable directory: every other mount is read-only, and a
    // rename does not cross mounts.
    SYS_rename,
#else
    SYS_renameat,
#endif
};

// Terminal requests ffmpeg makes of stdin, stdout and stderr to see whether they are terminals.
inline constexpr std::array<std::uint32_t, 2> kIoctls = {0x5401 /* TCGETS */,
                                                         0x5413 /* TIOCGWINSZ */};
// PR_SET_NAME names the threads it starts; PR_CAPBSET_READ is a library's probe of the
// bounding set, which is empty.
inline constexpr std::array<std::uint32_t, 2> kPrctls = {PR_SET_NAME, PR_CAPBSET_READ};
// F_GETFD, F_SETFD, F_GETFL, F_SETFL and F_DUPFD_CLOEXEC, on descriptors it already holds.
inline constexpr std::array<std::uint32_t, 5> kFcntls = {F_GETFD, F_SETFD, F_GETFL, F_SETFL,
                                                         F_DUPFD_CLOEXEC};

// Every CLONE_NEW* flag. CLONE_NEWTIME is the one in the low word that no header above 5.6
// carries.
inline constexpr std::uint32_t kNewNamespaces = CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS |
                                                CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID |
                                                CLONE_NEWNET | 0x00000080U;
// A thread, and nothing else: threads share the address space, the signal handlers and the
// thread group, and a clone without them would make a process.
inline constexpr std::uint32_t kThreadFlags = CLONE_VM | CLONE_SIGHAND | CLONE_THREAD;

inline constexpr std::array<std::uint32_t, 1> kAbort = {SIGABRT};

inline constexpr std::uint32_t kAllow = SECCOMP_RET_ALLOW;
inline constexpr std::uint32_t kKill = SECCOMP_RET_KILL_PROCESS;
// clone3 takes its flags from memory, where a filter cannot read them. ENOSYS is what glibc
// answers with the older clone, whose flags are in registers and are checked below.
inline constexpr std::uint32_t kNoSys = SECCOMP_RET_ERRNO | static_cast<std::uint32_t>(ENOSYS);

namespace detail {

constexpr std::uint32_t kNrOffset = offsetof(seccomp_data, nr);
constexpr std::uint32_t kArchOffset = offsetof(seccomp_data, arch);

constexpr std::uint32_t number(int nr) {
    return static_cast<std::uint32_t>(nr);
}

constexpr std::uint32_t arg_low(unsigned index) {
    return static_cast<std::uint32_t>(offsetof(seccomp_data, args) + (index * sizeof(__u64)));
}
constexpr std::uint32_t arg_high(unsigned index) {
    return arg_low(index) + static_cast<std::uint32_t>(sizeof(__u32));
}

// Jump offsets in a BPF instruction are eight bits, and the chain below is longer than that,
// so every branch is a conditional over an unconditional jump, whose offset has 32.
class Assembler {
public:
    using Label = std::size_t;

    [[nodiscard]] Label make_label() {
        bound_.push_back(0);
        return bound_.size() - 1;
    }
    void bind(Label label) { bound_[label] = code_.size(); }

    void load(std::uint32_t offset) { emit(BPF_LD | BPF_W | BPF_ABS, 0, 0, offset); }
    void mask(std::uint32_t bits) { emit(BPF_ALU | BPF_AND | BPF_K, 0, 0, bits); }
    void jump_if_equal(std::uint32_t value, Label target) {
        emit(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, value);
        jump(target);
    }
    void jump_unless_equal(std::uint32_t value, Label target) {
        emit(BPF_JMP | BPF_JEQ | BPF_K, 1, 0, value);
        jump(target);
    }
    void jump(Label target) {
        fixups_.push_back({.at = code_.size(), .target = target});
        emit(BPF_JMP | BPF_JA, 0, 0, 0);
    }
    void ret(std::uint32_t verdict) { emit(BPF_RET | BPF_K, 0, 0, verdict); }

    [[nodiscard]] std::vector<sock_filter> finish() {
        for (const Fixup& fixup : fixups_) {
            code_[fixup.at].k = static_cast<std::uint32_t>(bound_[fixup.target] - fixup.at - 1);
        }
        return std::move(code_);
    }

private:
    struct Fixup {
        std::size_t at;
        Label target;
    };

    void emit(std::uint16_t code, std::uint8_t jt, std::uint8_t jf, std::uint32_t k) {
        code_.push_back({.code = code, .jt = jt, .jf = jf, .k = k});
    }

    std::vector<sock_filter> code_;
    std::vector<Fixup> fixups_;
    std::vector<std::size_t> bound_;
};

} // namespace detail

// The whole filter. Layout: refuse foreign architectures, the unconditional calls, then one
// block per call allowed only with particular arguments, each of which kills on other
// arguments, and last the verdicts everything jumps to.
[[nodiscard]] inline std::vector<sock_filter> build_program() {
    detail::Assembler a;
    const auto allow = a.make_label();
    const auto kill = a.make_label();
    const auto no_sys = a.make_label();

    // A call from another ABI (32-bit on a 64-bit kernel) has its own numbering, in which an
    // allowed number here is a different call. The x32 ABI keeps the architecture and marks
    // its numbers with bit 30, so none of them equals an entry of the table.
    a.load(detail::kArchOffset);
    a.jump_unless_equal(kAudit, kill);
    a.load(detail::kNrOffset);
    for (const int nr : kAllowed) {
        a.jump_if_equal(detail::number(nr), allow);
    }
    a.jump_if_equal(detail::number(SYS_clone3), no_sys);

    const auto allow_arguments_in = [&](int nr, unsigned index,
                                        std::span<const std::uint32_t> values) {
        const auto next = a.make_label();
        a.load(detail::kNrOffset);
        a.jump_unless_equal(detail::number(nr), next);
        a.load(detail::arg_low(index));
        for (const std::uint32_t value : values) {
            a.jump_if_equal(value, allow);
        }
        a.jump(kill);
        a.bind(next);
    };
    allow_arguments_in(SYS_ioctl, 1, kIoctls);
    allow_arguments_in(SYS_prctl, 0, kPrctls);
    allow_arguments_in(SYS_fcntl, 1, kFcntls);
    // abort() raises SIGABRT with tgkill, and __stack_chk_fail, malloc's checks and av_assert
    // all end there. Only that signal: a filter cannot tell that the target is the caller's own
    // process (the pid is an argument, not something it can compare with getpid), so a decoder
    // could send SIGABRT to another process of this namespace, which is only ever the helper.
    allow_arguments_in(SYS_tgkill, 2, kAbort);
    allow_arguments_in(SYS_tkill, 1, kAbort);

    // prlimit64(0, resource, NULL, old): reading this process's own limits, as the C library
    // does for the thread stack size. Setting one is not.
    const auto after_prlimit = a.make_label();
    a.load(detail::kNrOffset);
    a.jump_unless_equal(detail::number(SYS_prlimit64), after_prlimit);
    a.load(detail::arg_low(0));
    a.jump_unless_equal(0, kill);
    a.load(detail::arg_low(2));
    a.jump_unless_equal(0, kill);
    a.load(detail::arg_high(2));
    a.jump_unless_equal(0, kill);
    a.jump(allow);
    a.bind(after_prlimit);

    // clone: a thread, in no new namespace. Its flags are one register, unsigned long, and the
    // kernel reads the low 32 bits of the namespace and thread flags.
    const auto after_clone = a.make_label();
    a.load(detail::kNrOffset);
    a.jump_unless_equal(detail::number(SYS_clone), after_clone);
    a.load(detail::arg_low(0));
    a.mask(kNewNamespaces);
    a.jump_unless_equal(0, kill);
    a.load(detail::arg_low(0));
    a.mask(kThreadFlags);
    a.jump_if_equal(kThreadFlags, allow);
    a.jump(kill);
    a.bind(after_clone);

    a.jump(kill);
    a.bind(allow);
    a.ret(kAllow);
    a.bind(kill);
    a.ret(kKill);
    a.bind(no_sys);
    a.ret(kNoSys);
    return a.finish();
}

// After no_new_privs, which the helper sets when it drops its capabilities, and after every
// other confinement, since none of it can be done once the filter is on. LOG puts each kill in
// the audit log, where an operator looks for an exploit attempt.
[[nodiscard]] inline bool install() noexcept {
    std::vector<sock_filter> code;
    try {
        code = build_program();
    } catch (...) {
        errno = ENOMEM;
        return false;
    }
    sock_fprog program{.len = static_cast<unsigned short>(code.size()), .filter = code.data()};
    return ::syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_LOG, &program) == 0;
}

} // namespace infra::ffmpeg::seccomp
