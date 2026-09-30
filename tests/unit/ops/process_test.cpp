#include "os/unique_fd.hpp"

#include "ops/process.hpp"

#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include <cstdlib>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>
#include <vector>

namespace {

TEST(ProcessStats, OpeningDescriptorsRaisesTheCountByExactlyThatMany) {
    const auto before = ops::open_descriptors();
    ASSERT_TRUE(before);
    std::vector<os::UniqueFd> held;
    held.reserve(5);
    for (int i = 0; i < 5; ++i) {
        held.emplace_back(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    }
    EXPECT_EQ(ops::open_descriptors(), *before + 5);
    held.clear();
    EXPECT_EQ(ops::open_descriptors(), *before);
}

TEST(ProcessStats, ResidentMemoryGrowsWhenPagesAreTouched) {
    const auto before = ops::resident_bytes();
    ASSERT_TRUE(before);
    constexpr std::size_t kSize = std::size_t{64} << 20U;
    std::vector<char> block(kSize, 'x');
    const auto after = ops::resident_bytes();
    ASSERT_TRUE(after);
    // Most of 64 MiB must now be resident, whatever else the allocator did.
    EXPECT_GT(*after, *before + (kSize / 2));
    EXPECT_EQ(block.back(), 'x');
}

// In a child of its own: the change is for the whole process, and for good.
TEST(DisableCoreDumps, LeavesNoCoreLimitAndAProcessNobodyMayReadOrAttachTo) {
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        const rlimit open{.rlim_cur = RLIM_INFINITY, .rlim_max = RLIM_INFINITY};
        static_cast<void>(::setrlimit(RLIMIT_CORE, &open));
        if (!ops::disable_core_dumps()) {
            std::_Exit(1);
        }
        rlimit now{};
        if (::getrlimit(RLIMIT_CORE, &now) != 0 || now.rlim_cur != 0 || now.rlim_max != 0) {
            std::_Exit(2);
        }
        std::_Exit(::prctl(PR_GET_DUMPABLE) == 0 ? 0 : 3);
    }
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

} // namespace
