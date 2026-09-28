#include "infra/s3util/crypto.hpp"

#include "signing_key.hpp"

#include <gtest/gtest.h>
#include <utility>

namespace {

using infra::s3util::Sha256Digest;
using infra::s3util::detail::SigningKey;

Sha256Digest filled(unsigned char byte) {
    Sha256Digest key{};
    key.fill(byte);
    return key;
}

TEST(SigningKey, MoveConstructionLeavesZeroesBehind) {
    SigningKey source{filled(0xA5)};
    const SigningKey moved{std::move(source)};

    EXPECT_EQ(moved.bytes(), filled(0xA5));
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move): under test.
    EXPECT_EQ(source.bytes(), Sha256Digest{});
}

TEST(SigningKey, MoveAssignmentLeavesZeroesBehind) {
    SigningKey source{filled(0xA5)};
    SigningKey target{filled(0x3C)};
    target = std::move(source);

    EXPECT_EQ(target.bytes(), filled(0xA5));
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move): under test.
    EXPECT_EQ(source.bytes(), Sha256Digest{});
}

} // namespace
