#include "ops/process.hpp"
#include "ops/root.hpp"

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include <array>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <gtest/gtest.h>
#include <pwd.h>
#include <string>
#include <system_error>
#include <unistd.h>

namespace {

TEST(ParseAllowRoot, TakesZeroOrOneAndUnsetAsZero) {
    EXPECT_EQ(ops::parse_allow_root(std::nullopt), false);
    EXPECT_EQ(ops::parse_allow_root(""), false);
    EXPECT_EQ(ops::parse_allow_root("0"), false);
    EXPECT_EQ(ops::parse_allow_root("1"), true);
    EXPECT_EQ(ops::parse_allow_root("true"), std::nullopt);
    EXPECT_EQ(ops::parse_allow_root("yes"), std::nullopt);
    EXPECT_EQ(ops::parse_allow_root("01"), std::nullopt);
}

TEST(LeaveRoot, NotRootThereIsNothingToGiveUp) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "runs as root";
    }
    const uid_t before = ::getuid();
    EXPECT_EQ(ops::leave_root("", false), ops::RootStep::NotRoot);
    EXPECT_EQ(ops::leave_root("nobody", false), ops::RootStep::NotRoot);
    EXPECT_EQ(::getuid(), before);
}

// Ids change for the whole process, so each case runs in a child of its own, which returns 0
// for success.
int in_child(const std::function<int()>& body) {
    const pid_t child = ::fork();
    if (child == 0) {
        std::_Exit(body());
    }
    int status = 0;
    if (child < 0 || ::waitpid(child, &status, 0) != child) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

class LeaveRootTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (::geteuid() != 0) {
            GTEST_SKIP() << "needs root";
        }
    }
};

TEST_F(LeaveRootTest, RootWithNoUserIsAConfigurationErrorUnlessAllowed) {
    EXPECT_EQ(in_child([] {
                  const auto refused = ops::leave_root("", false);
                  if (refused || !refused.error().configuration ||
                      refused.error().source != "ULW_RUN_AS_USER" || ::getuid() != 0) {
                      return 1;
                  }
                  return ops::leave_root("", true) == ops::RootStep::StayedRoot && ::getuid() == 0
                             ? 0
                             : 2;
              }),
              0);
}

TEST_F(LeaveRootTest, ANamedUserIsBecomeAndAnUnknownOneRefused) {
    const passwd* nobody = ::getpwnam("nobody");
    if (nobody == nullptr) {
        GTEST_SKIP() << "no nobody user on this host";
    }
    const uid_t uid = nobody->pw_uid;
    EXPECT_EQ(in_child([] {
                  const auto unknown = ops::leave_root("no-such-user-in-any-passwd", false);
                  return !unknown && unknown.error().configuration && ::getuid() == 0 ? 0 : 1;
              }),
              0);
    EXPECT_EQ(in_child([uid] {
                  return ops::leave_root("nobody", false) == ops::RootStep::Dropped &&
                                 ::getuid() == uid && ::geteuid() == uid
                             ? 0
                             : 1;
              }),
              0);
}

// A change of uid sets the flag to fs.suid_dumpable. On a host where that is 1 or 2 (Ubuntu's
// apport and systemd-coredump set 2) this fails without the step in leave_root that puts the flag
// back; where it is 0 the kernel itself leaves the flag off, and the next test is the one that
// shows the step is taken.
TEST_F(LeaveRootTest, AProcessThatTurnedDumpsOffKeepsThemOffAfterTheDrop) {
    if (::getpwnam("nobody") == nullptr) {
        GTEST_SKIP() << "no nobody user on this host";
    }
    EXPECT_EQ(in_child([] {
                  if (!ops::disable_core_dumps() || ::prctl(PR_GET_DUMPABLE) != 0) {
                      return 1;
                  }
                  if (ops::leave_root("nobody", false) != ops::RootStep::Dropped) {
                      return 2;
                  }
                  return ::prctl(PR_GET_DUMPABLE) == 0 ? 0 : 3;
              }),
              0);
}

#if defined(__x86_64__)
constexpr std::uint32_t kAuditArch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
constexpr std::uint32_t kAuditArch = AUDIT_ARCH_AARCH64;
#else
#error "no seccomp audit architecture for this target"
#endif

// Makes prctl(PR_SET_DUMPABLE, ...) fail with EPERM for this process from now on, and lets every
// other call through. Returns false when the filter could not be installed.
bool refuse_set_dumpable() {
    constexpr auto kArch = static_cast<std::uint32_t>(offsetof(seccomp_data, arch));
    constexpr auto kNr = static_cast<std::uint32_t>(offsetof(seccomp_data, nr));
    // The low word of the first argument, on a little-endian target.
    constexpr auto kOption = static_cast<std::uint32_t>(offsetof(seccomp_data, args));
    static_assert(std::endian::native == std::endian::little);
    std::array<sock_filter, 8> code{{
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, kArch),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kAuditArch, 0, 5),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, kNr),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_prctl, 0, 3),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, kOption),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, PR_SET_DUMPABLE, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | static_cast<std::uint32_t>(EPERM)),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    }};
    const sock_fprog program{.len = code.size(), .filter = code.data()};
    return ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
}

// Whatever fs.suid_dumpable is: the flag is put back after the drop, and a failure to put it back
// refuses the drop with the errno, rather than serving dumpable.
TEST_F(LeaveRootTest, ADropThatCannotTurnDumpsBackOffIsRefused) {
    if (::getpwnam("nobody") == nullptr) {
        GTEST_SKIP() << "no nobody user on this host";
    }
    EXPECT_EQ(in_child([] {
                  if (!ops::disable_core_dumps() || !refuse_set_dumpable()) {
                      return 1;
                  }
                  const auto step = ops::leave_root("nobody", false);
                  if (step) {
                      return 2;
                  }
                  const bool as_expected =
                      !step.error().configuration && step.error().source == "PR_SET_DUMPABLE" &&
                      step.error().reason == std::generic_category().message(EPERM);
                  return as_expected ? 0 : 3;
              }),
              0);
}

} // namespace
