#include "core/errors/domain_error.hpp"
#include "core/models/content_type.hpp"

#include <expected>
#include <gtest/gtest.h>
#include <string>
#include <string_view>

namespace {

using core::ContentType;
using core::DomainError;

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

TEST(ContentType, RejectsMediaRanges) {
    for (const std::string_view t : {"*/*", "video/*", "*/mp4", "video/mp*"}) {
        EXPECT_EQ(ContentType::parse(t), std::unexpected(DomainError::InvalidContentType)) << t;
    }
}

TEST(ContentType, EnforcesTheLengthBoundExactly) {
    const std::string longest = "video/" + std::string(ContentType::kMaxLength - 6, 'x');
    ASSERT_EQ(longest.size(), ContentType::kMaxLength);
    EXPECT_TRUE(ContentType::parse(longest).has_value());
    EXPECT_EQ(ContentType::parse(longest + "x"), std::unexpected(DomainError::InvalidContentType));
}

} // namespace
