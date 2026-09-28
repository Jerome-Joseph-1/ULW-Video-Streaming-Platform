#include "core/models/content_type.hpp"
#include "core/models/storage_key.hpp"

#include <gtest/gtest.h>
#include <string>

namespace {

using core::ContentType;
using core::DomainError;
using core::StorageKey;

TEST(StorageKey, AcceptsKeysUsedByThePipelineUnchanged) {
    for (const std::string_view k :
         {"a/b/c", "video-01J8/raw", "a.b_c-d/1",
          "hls/0192f3c4-7a1b-7c2d-8e3f-0123456789ab/720p/seg_00001.m4s"}) {
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
        EXPECT_FALSE(StorageKey::parse(k).has_value()) << k;
    }
}

TEST(StorageKey, EnforcesTheLengthBoundExactly) {
    EXPECT_TRUE(StorageKey::parse(std::string(StorageKey::kMaxLength, 'k')).has_value());
    EXPECT_FALSE(StorageKey::parse(std::string(StorageKey::kMaxLength + 1, 'k')).has_value());
}

TEST(ContentType, LowercasesAValidMediaType) {
    const auto t = ContentType::parse("Video/MP4");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->view(), "video/mp4");
}

TEST(ContentType, RejectsParametersAndMalformedTypes) {
    for (const std::string_view t : {"", "video", "video/", "/mp4", "video/mp4; codecs=avc1",
                                     "video/mp4/x", "vid eo/mp4", "video/mp4\r\nX-Evil: 1"}) {
        EXPECT_EQ(ContentType::parse(t), std::unexpected(DomainError::InvalidContentType)) << t;
    }
}

} // namespace
