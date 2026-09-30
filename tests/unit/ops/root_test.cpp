#include "ops/process.hpp"
#include "ops/root.hpp"

#include <sys/prctl.h>
#include <sys/wait.h>

#include <cstdlib>
#include <functional>
#include <gtest/gtest.h>
#include <pwd.h>
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

// A change of uid sets the flag to fs.suid_dumpable, which a host may set to 1 or 2.
TEST_F(LeaveRootTest, AProcessThatTurnedDumpsOffKeepsThemOffAfterTheDrop) {
    if (::getpwnam("nobody") == nullptr) {
        GTEST_SKIP() << "no nobody user on this host";
    }
    EXPECT_EQ(in_child([] {
                  if (!ops::disable_core_dumps()) {
                      return 1;
                  }
                  if (ops::leave_root("nobody", false) != ops::RootStep::Dropped) {
                      return 2;
                  }
                  return ::prctl(PR_GET_DUMPABLE) == 0 ? 0 : 3;
              }),
              0);
}

} // namespace
