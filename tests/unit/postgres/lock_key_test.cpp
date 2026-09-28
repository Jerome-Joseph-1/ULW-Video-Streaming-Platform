#include "core/models/ids.hpp"

#include "lock_key.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <string_view>

namespace {

using infra::postgres::lock_key;

core::UploadId upload(std::string_view text) {
    return *core::UploadId::parse(text);
}

TEST(LockKey, TakesTheRandomTailBelowTheVariantBitsAndOneBitOfRandA) {
    // Tail bc ce b3 02 09 9a 80 57 with the variant bits cleared is 3cceb302099a8057; the low
    // bit of byte 7 (0x4b) lands in bit 62.
    EXPECT_EQ(lock_key(upload("01890a5d-ac96-774b-bcce-b302099a8057")),
              std::int64_t{0x7cceb302099a8057});
}

TEST(LockKey, IgnoresTheTimestampHead) {
    EXPECT_EQ(lock_key(upload("01890a5d-ac96-774b-bcce-b302099a8057")),
              lock_key(upload("0190ffff-ffff-704b-bcce-b302099a8057")));
}

TEST(LockKey, SeparatesUploadsThatDifferOnlyInTheirRandomBits) {
    const auto base = lock_key(upload("01890a5d-ac96-774b-bcce-b302099a8057"));
    EXPECT_NE(base, lock_key(upload("01890a5d-ac96-774a-bcce-b302099a8057")));
    EXPECT_NE(base, lock_key(upload("01890a5d-ac96-774b-bcce-b302099a8056")));
    EXPECT_NE(base, lock_key(upload("01890a5d-ac96-774b-8cce-b302099a8057")));
}

TEST(LockKey, IsNeverNegative) {
    EXPECT_EQ(lock_key(upload("ffffffff-ffff-7fff-ffff-ffffffffffff")),
              std::numeric_limits<std::int64_t>::max());
}

} // namespace
