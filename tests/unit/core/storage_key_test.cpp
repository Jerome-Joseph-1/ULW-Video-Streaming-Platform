#include "core/errors/domain_error.hpp"
#include "core/models/storage_key.hpp"

#include <expected>
#include <gtest/gtest.h>
#include <string>
#include <string_view>

namespace {

using core::DomainError;
using core::StorageKey;

TEST(StorageKey, AcceptsKeysUsedByThePipelineUnchanged) {
    for (const std::string_view k :
         {"a/b/c", "video-01J8/raw", "a.b_c-d/1",
          "hls/0192f3c4-7a1b-7c2d-8e3f-0123456789ab/720p/seg_00001.m4s",
          // Each end of every range, and segments that start with a symbol.
          "AZaz09/-first/_under/.hidden/Z/A"}) {
        const auto key = StorageKey::parse(k);
        ASSERT_TRUE(key.has_value()) << k;
        EXPECT_EQ(key->view(), k);
    }
}

TEST(StorageKey, RejectsTraversalAndEmptySegments) {
    for (const std::string_view k : {"", "/a", "a/", "a//b", "..", "a/../b", "./a", "a/./b"}) {
        EXPECT_EQ(StorageKey::parse(k), std::unexpected(DomainError::InvalidStorageKey)) << k;
    }
}

TEST(StorageKey, RejectsCharactersThatNeedEscaping) {
    for (const std::string_view k : {"a b", "a?b", "a%2Fb", "a\\b", "caf\xc3\xa9", "a\nb"}) {
        EXPECT_EQ(StorageKey::parse(k), std::unexpected(DomainError::InvalidStorageKey)) << k;
    }
}

TEST(StorageKey, EnforcesTheLengthBoundExactly) {
    EXPECT_TRUE(StorageKey::parse(std::string(StorageKey::kMaxLength, 'k')).has_value());
    EXPECT_EQ(StorageKey::parse(std::string(StorageKey::kMaxLength + 1, 'k')),
              std::unexpected(DomainError::InvalidStorageKey));
}

} // namespace
