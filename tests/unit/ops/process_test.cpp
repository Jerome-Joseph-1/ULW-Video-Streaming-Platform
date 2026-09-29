#include "os/unique_fd.hpp"

#include "ops/process.hpp"

#include <fcntl.h>
#include <gtest/gtest.h>
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

} // namespace
