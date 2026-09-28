#include "core/version.hpp"

#include <gtest/gtest.h>

namespace {

TEST(BuildInfo, VersionMatchesProjectVersion) {
    EXPECT_EQ(core::build_info().version, ULW_EXPECTED_VERSION);
}

TEST(BuildInfo, GitShaIsStampedFromTheCheckout) {
    const auto sha = core::build_info().git_sha;
    ASSERT_NE(sha, "unknown");
    EXPECT_GE(sha.size(), 12U);
}

} // namespace
