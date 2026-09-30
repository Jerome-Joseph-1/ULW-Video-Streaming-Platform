// The child process harness, as the process tests use it to wait on a program's state.
#include "support/child_process.hpp"
#include "support/eventually.hpp"

#include <sys/types.h>

#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

namespace {

using std::chrono::milliseconds;

// Readiness checks that each open a connection were once made in a spinning loop, several
// thousand a second, which filled the host's connection tracking and TIME_WAIT slots while a
// node was still starting. poll_until asks at most once a period, waiting on the program's
// output in between.
TEST(ChildProcess, PollUntilAsksAtMostOncePerPeriod) {
    // Quiet and alive until killed: the wait between checks is the whole period.
    const auto quiet = ulw::test::ChildProcess::start({"/usr/bin/tail", "-f", "/dev/null"}, {});
    ASSERT_TRUE(quiet);
    int asked = 0;
    const milliseconds limit{600};
    const milliseconds period{200};
    EXPECT_FALSE(quiet->poll_until(
        [&] {
            ++asked;
            return false;
        },
        limit, period));
    EXPECT_GE(asked, 2);
    EXPECT_LE(asked, static_cast<int>(limit / period) + 1);
}

// The datagram soak measures a leak by its RSS, which transparent huge pages grow with nothing
// allocated: where THP is "always", khugepaged collapses a partly touched 2 MiB range into a huge
// page mid-run (a 1,528 KiB step failed the 1-byte bound on a runner). The soak turns THP off for
// itself before it allocates anything.
TEST(DatagramSoak, RunsWithTransparentHugePagesOff) {
    // Epoll, so that a host without io_uring runs it as far as the check.
    const auto soak =
        ulw::test::ChildProcess::start({ULW_DATAGRAM_SOAK_BIN, "--reactor", "epoll", "--peers", "4",
                                        "--duration-s", "60", "--sample-s", "30"},
                                       {});
    ASSERT_TRUE(soak);
    const auto thp_enabled = [pid = soak->pid()]() -> std::string {
        std::ifstream status("/proc/" + std::to_string(pid) + "/status");
        std::string line;
        while (std::getline(status, line)) {
            if (line.starts_with("THP_enabled:")) {
                return line;
            }
        }
        return {};
    };
    EXPECT_TRUE(ulw::test::eventually([&] { return thp_enabled() == "THP_enabled:\t0"; }))
        << "last read: \"" << thp_enabled() << "\"\n"
        << soak->output();
}

} // namespace
