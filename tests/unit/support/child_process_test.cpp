// The child process harness, as the process tests use it to wait on a program's state.
#include "support/child_process.hpp"
#include "support/eventually.hpp"

#include <chrono>
#include <gtest/gtest.h>

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

} // namespace
