#include "upload_claim.hpp"

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using core::ports::CatalogCallback;
using core::ports::CatalogError;
using core::ports::StoredUpload;
using gateway::UploadClaim;

// Records what is released and nothing else: UploadClaim only ever releases.
class ReleaseLog final : public core::ports::IUploadCatalog {
public:
    void create_upload(core::ports::NewUpload /*upload*/, CatalogCallback<void> done) override {
        done(std::unexpected(CatalogError::Unavailable));
    }
    void find_upload(const core::UploadId& /*id*/, CatalogCallback<StoredUpload> done) override {
        done(std::unexpected(CatalogError::Unavailable));
    }
    void claim_upload(const core::UploadId& /*id*/, const core::UserId& /*owner*/,
                      CatalogCallback<StoredUpload> done) override {
        done(std::unexpected(CatalogError::Unavailable));
    }
    void release_upload(const core::UploadId& id) noexcept override { released.push_back(id); }
    void record_progress(const core::UploadId& /*id*/, const core::VideoId& /*video*/,
                         std::uint64_t /*durable_offset*/, CatalogCallback<void> done) override {
        done(std::unexpected(CatalogError::Unavailable));
    }
    void commit_upload(const core::UploadId& /*id*/, const core::VideoId& /*video*/,
                       const std::string& /*request_id*/,
                       CatalogCallback<core::VideoState> done) override {
        done(std::unexpected(CatalogError::Unavailable));
    }
    void abort_upload(const core::UploadId& /*id*/, CatalogCallback<void> done) override {
        done(std::unexpected(CatalogError::Unavailable));
    }
    void find_video(const core::VideoId& /*id*/, CatalogCallback<core::VideoRecord> done) override {
        done(std::unexpected(CatalogError::Unavailable));
    }

    std::vector<core::UploadId> released;
};

core::UploadId upload(std::string_view text) {
    return *core::UploadId::parse(text);
}

const core::UploadId kFirst = upload("0192f3c4-7a1b-7c2d-8e3f-0123456789ab");
const core::UploadId kSecond = upload("0192f3c4-7a1b-7c2d-8e3f-0123456789ac");

TEST(UploadClaim, AReleaseForTheHolderGivesTheClaimBackOnce) {
    ReleaseLog catalog;
    std::size_t held = 0;
    UploadClaim claim(catalog, held);
    claim.adopt(1, kFirst);
    EXPECT_TRUE(claim.held());
    EXPECT_TRUE(claim.held_by(1));
    EXPECT_EQ(held, 1U);
    claim.release(1);
    claim.release(1);
    EXPECT_FALSE(claim.held());
    EXPECT_EQ(held, 0U);
    EXPECT_EQ(catalog.released, std::vector{kFirst});
}

// The audit's case: a completion for request 1 arrives after request 1 ended and request 2
// claimed another upload on the same connection. It must leave request 2's claim alone.
TEST(UploadClaim, AStaleCompletionCannotReleaseALaterRequestsClaim) {
    ReleaseLog catalog;
    std::size_t held = 0;
    UploadClaim claim(catalog, held);
    claim.adopt(1, kFirst);
    claim.release(1);
    claim.adopt(2, kSecond);
    claim.release(1);
    EXPECT_TRUE(claim.held_by(2));
    EXPECT_FALSE(claim.held_by(1));
    EXPECT_EQ(held, 1U);
    EXPECT_EQ(catalog.released, std::vector{kFirst});
    claim.release(2);
    EXPECT_EQ(held, 0U);
    EXPECT_EQ(catalog.released, (std::vector{kFirst, kSecond}));
}

// The fragile case: a request that ended without giving its claim back. The next request's
// claim is not stacked on top of it: the old one is released, and counted out, first.
TEST(UploadClaim, AClaimLeftByAFinishedRequestGoesWhenTheNextOneClaims) {
    ReleaseLog catalog;
    std::size_t held = 0;
    UploadClaim claim(catalog, held);
    claim.adopt(1, kFirst);
    claim.adopt(2, kSecond);
    EXPECT_TRUE(claim.held_by(2));
    EXPECT_EQ(held, 1U);
    EXPECT_EQ(catalog.released, std::vector{kFirst});
}

TEST(UploadClaim, ReleasingAnyFreesWhoeverHoldsIt) {
    ReleaseLog catalog;
    std::size_t held = 0;
    UploadClaim claim(catalog, held);
    claim.release_any();
    EXPECT_TRUE(catalog.released.empty());
    claim.adopt(7, kFirst);
    claim.release_any();
    EXPECT_FALSE(claim.held());
    EXPECT_EQ(held, 0U);
    EXPECT_EQ(catalog.released, std::vector{kFirst});
}

TEST(UploadClaim, AClaimStillHeldIsReleasedWhenItsHolderGoes) {
    ReleaseLog catalog;
    std::size_t held = 0;
    {
        UploadClaim claim(catalog, held);
        claim.adopt(3, kSecond);
        EXPECT_EQ(held, 1U);
    }
    EXPECT_EQ(held, 0U);
    EXPECT_EQ(catalog.released, std::vector{kSecond});
}

TEST(UploadClaim, ClaimsOnSeveralConnectionsShareOneCount) {
    ReleaseLog catalog;
    std::size_t held = 0;
    UploadClaim a(catalog, held);
    UploadClaim b(catalog, held);
    a.adopt(1, kFirst);
    b.adopt(1, kSecond);
    EXPECT_EQ(held, 2U);
    a.release(1);
    EXPECT_EQ(held, 1U);
    EXPECT_TRUE(b.held_by(1));
}

} // namespace
