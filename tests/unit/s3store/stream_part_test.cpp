#include "infra/storage/s3_transfer.hpp"

#include <cstdint>
#include <gtest/gtest.h>

namespace {

using infra::storage::S3Transfer;

constexpr std::uint64_t kMiB = std::uint64_t{1} << 20U;

TEST(StreamPartBytes, ASmallStreamGoesUpInSixteenMebibyteParts) {
    EXPECT_EQ(S3Transfer::stream_part_bytes(0), 16 * kMiB);
    EXPECT_EQ(S3Transfer::stream_part_bytes(kMiB * 16 * 9'000), 16 * kMiB);
}

TEST(StreamPartBytes, ALargerBoundFitsInNineThousandPartsOfWholeMebibytes) {
    // One byte past 9,000 parts of 16 MiB needs the next whole MiB.
    EXPECT_EQ(S3Transfer::stream_part_bytes((kMiB * 16 * 9'000) + 1), 17 * kMiB);
    // 12 h at 100 Mbit/s, the packager's ceiling, with an eighth more for the container.
    const std::uint64_t longest = std::uint64_t{100'000} * 125 * 43'200 * 9 / 8;
    const std::uint64_t part = S3Transfer::stream_part_bytes(longest);
    EXPECT_EQ(part % kMiB, 0U);
    EXPECT_GE(part * 9'000, longest);
    EXPECT_LT((part - kMiB) * 9'000, longest);
    EXPECT_EQ(part, 65 * kMiB);
}

} // namespace
