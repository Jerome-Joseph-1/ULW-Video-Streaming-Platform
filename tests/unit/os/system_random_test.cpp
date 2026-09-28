#include "os/system_random.hpp"

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <gtest/gtest.h>
#include <span>
#include <vector>

namespace {

// Larger than the 256 bytes getrandom(2) guarantees per call, so short reads must be resumed.
constexpr std::size_t kBufferSize = std::size_t{64} << 10U;
constexpr std::size_t kBlockSize = std::size_t{4} << 10U;

TEST(SystemRandom, FillsEveryBlockOfALargeBuffer) {
    os::SystemRandom random;
    std::vector<std::byte> buffer(kBufferSize, std::byte{0});
    random.fill(buffer);
    // An unfilled 4 KiB block stays all zero; a filled one is constant with probability 2^-32760.
    for (std::size_t at = 0; at < buffer.size(); at += kBlockSize) {
        const auto block = std::span(buffer).subspan(at, kBlockSize);
        EXPECT_FALSE(std::ranges::all_of(block, [&](std::byte b) { return b == block.front(); }))
            << "block at " << at;
    }
    // 64 KiB of uniform bytes miss one of the 256 values with probability about 256 * e^-256.
    std::bitset<256> seen;
    for (const std::byte b : buffer) {
        seen.set(std::to_integer<std::size_t>(b));
    }
    EXPECT_TRUE(seen.all());
}

TEST(SystemRandom, SuccessiveFillsDiffer) {
    os::SystemRandom random;
    std::vector<std::byte> first(kBlockSize);
    std::vector<std::byte> second(kBlockSize);
    random.fill(first);
    random.fill(second);
    EXPECT_NE(first, second);
}

} // namespace
