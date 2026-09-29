#include "os/privileges.hpp"

#include <sys/prctl.h>
#include <sys/wait.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <grp.h>
#include <gtest/gtest.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

// The ids of "nobody" and "nogroup" on Debian and Ubuntu; nothing here needs them to exist,
// only to differ from root and from each other.
constexpr os::Identity kTarget{.uid = 65534, .gid = 65533};
constexpr os::Identity kOther{.uid = 65532, .gid = 65531};

// Ids change for the whole process, so each case runs in a child of its own. The body returns
// 0 for success; any other value, or a crash, fails the case with that status.
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

class DropPrivilegesTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!os::is_root()) {
            GTEST_SKIP() << "changing to arbitrary ids needs root";
        }
    }
};

TEST_F(DropPrivilegesTest, LeavesEveryIdAtTheTargetAndRootUnreachable) {
    EXPECT_EQ(in_child([] {
                  if (!os::drop_privileges(kTarget)) {
                      return 1;
                  }
                  uid_t r = 1;
                  uid_t e = 1;
                  uid_t s = 1;
                  gid_t rg = 1;
                  gid_t eg = 1;
                  gid_t sg = 1;
                  ::getresuid(&r, &e, &s);
                  ::getresgid(&rg, &eg, &sg);
                  if (r != kTarget.uid || e != kTarget.uid || s != kTarget.uid) {
                      return 2;
                  }
                  if (rg != kTarget.gid || eg != kTarget.gid || sg != kTarget.gid) {
                      return 3;
                  }
                  return ::setuid(0) != 0 && ::setgid(0) != 0 && ::seteuid(0) != 0 ? 0 : 4;
              }),
              0);
}

TEST_F(DropPrivilegesTest, ClearsSupplementaryGroupsInherited) {
    EXPECT_EQ(in_child([] {
                  const std::vector<gid_t> groups{0, 4, 27};
                  if (::setgroups(groups.size(), groups.data()) != 0 ||
                      ::getgroups(0, nullptr) != 3) {
                      return 1;
                  }
                  if (!os::drop_privileges(kTarget)) {
                      return 2;
                  }
                  return ::getgroups(0, nullptr) == 0 ? 0 : 3;
              }),
              0);
}

TEST_F(DropPrivilegesTest, HoldsNoCapabilityAfterwards) {
    EXPECT_EQ(in_child([] {
                  if (!os::drop_privileges(kTarget)) {
                      return 1;
                  }
                  // A capability shows in the effective set of /proc/self/status.
                  std::ifstream status("/proc/self/status");
                  std::string line;
                  while (std::getline(status, line)) {
                      if (line.starts_with("CapEff:")) {
                          return line.ends_with("0000000000000000") ? 0 : 2;
                      }
                  }
                  return 3;
              }),
              0);
}

TEST_F(DropPrivilegesTest, NoLaterExecCanRaisePrivileges) {
    if (::prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 0) {
        GTEST_SKIP() << "no_new_privs is inherited already, so the drop cannot be seen setting it";
    }
    EXPECT_EQ(in_child([] {
                  if (!os::drop_privileges(kTarget)) {
                      return 2;
                  }
                  return ::prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1 ? 0 : 3;
              }),
              0);
}

TEST_F(DropPrivilegesTest, RefusesRootAsATargetAndChangesNothing) {
    EXPECT_EQ(in_child([] {
                  const auto uid = os::drop_privileges({.uid = 0, .gid = kTarget.gid});
                  const auto gid = os::drop_privileges({.uid = kTarget.uid, .gid = 0});
                  return !uid && !gid && ::getuid() == 0 && ::getgid() == 0 ? 0 : 1;
              }),
              0);
}

TEST_F(DropPrivilegesTest, FailsWhenAlreadyUnprivilegedAndTheTargetDiffers) {
    EXPECT_EQ(in_child([] {
                  if (!os::drop_privileges(kTarget)) {
                      return 1;
                  }
                  // Without CAP_SETGID the first step is refused, before any id moves again.
                  const auto again = os::drop_privileges(kOther);
                  return !again && ::getuid() == kTarget.uid && ::getgid() == kTarget.gid ? 0 : 2;
              }),
              0);
}

TEST_F(DropPrivilegesTest, DropsToANamedUser) {
    const auto nobody = os::resolve_user("nobody");
    if (!nobody) {
        GTEST_SKIP() << "no nobody user on this host";
    }
    EXPECT_EQ(in_child([&nobody] {
                  return os::drop_to_user("nobody") && ::getuid() == nobody->uid &&
                                 ::getgid() == nobody->gid
                             ? 0
                             : 1;
              }),
              0);
}

TEST(ResolveUserTest, FindsRootAndItsGroup) {
    const auto root = os::resolve_user("root");
    ASSERT_TRUE(root);
    EXPECT_EQ(root->uid, 0U);
    EXPECT_EQ(root->gid, 0U);
}

TEST(ResolveUserTest, ReportsAnUnknownUser) {
    const auto missing = os::resolve_user("no-such-user-in-any-passwd");
    ASSERT_FALSE(missing);
    EXPECT_NE(missing.error().find("no such user"), std::string::npos);
}

TEST(DropToUserTest, RefusesAnUnknownUserWithoutChangingIds) {
    const uid_t before = ::getuid();
    EXPECT_FALSE(os::drop_to_user("no-such-user-in-any-passwd"));
    EXPECT_EQ(::getuid(), before);
}

} // namespace
