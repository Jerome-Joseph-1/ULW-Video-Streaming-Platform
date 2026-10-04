#include "infra/catalog/memory_catalog.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "../conformance/storage_harness.hpp"
#include "support/reactor_harness.hpp"
#include "upload_claim.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using core::ports::CatalogCallback;
using core::ports::CatalogError;
using core::ports::ClaimedUpload;
using core::ports::ClaimToken;
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
                      CatalogCallback<ClaimedUpload> done) override {
        done(std::unexpected(CatalogError::Unavailable));
    }
    void release_upload(const core::UploadId& id, ClaimToken token) noexcept override {
        released.push_back(id);
        tokens.push_back(token);
    }
    void record_progress(const core::UploadId& /*id*/, ClaimToken /*token*/,
                         const core::VideoId& /*video*/, std::uint64_t /*durable_offset*/,
                         CatalogCallback<void> done) override {
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
    void find_video_for(const core::VideoId& /*id*/, const core::UserId& /*viewer*/,
                        CatalogCallback<core::ports::VideoView> done) override {
        done(std::unexpected(CatalogError::Unavailable));
    }
    void set_visibility(const core::VideoId& /*id*/, const core::UserId& /*owner*/,
                        const core::Visibility& /*visibility*/,
                        CatalogCallback<core::VideoRecord> done) override {
        done(std::unexpected(CatalogError::Unavailable));
    }
    void grant_access(const core::VideoId& /*id*/, const core::UserId& /*user*/,
                      CatalogCallback<void> done) override {
        done(std::unexpected(CatalogError::Unavailable));
    }
    void revoke_access(const core::VideoId& /*id*/, const core::UserId& /*user*/,
                       CatalogCallback<void> done) override {
        done(std::unexpected(CatalogError::Unavailable));
    }
    void list_grants(const core::VideoId& /*id*/, std::optional<core::UserId> /*after*/,
                     std::size_t /*limit*/, CatalogCallback<core::ports::GrantPage> done) override {
        done(std::unexpected(CatalogError::Unavailable));
    }

    std::vector<core::UploadId> released;
    std::vector<ClaimToken> tokens;
};

core::UploadId upload(std::string_view text) {
    return *core::UploadId::parse(text);
}

const core::UploadId kFirst = upload("0192f3c4-7a1b-7c2d-8e3f-0123456789ab");
const core::UploadId kSecond = upload("0192f3c4-7a1b-7c2d-8e3f-0123456789ac");
constexpr ClaimToken kOne{1};
constexpr ClaimToken kTwo{2};

TEST(UploadClaim, AReleaseForTheHolderGivesTheClaimBackOnce) {
    ReleaseLog catalog;
    std::size_t held = 0;
    UploadClaim claim(catalog, held);
    claim.adopt(1, kFirst, kOne);
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
    claim.adopt(1, kFirst, kOne);
    claim.release(1);
    claim.adopt(2, kSecond, kTwo);
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
    claim.adopt(1, kFirst, kOne);
    claim.adopt(2, kSecond, kTwo);
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
    claim.adopt(7, kFirst, kOne);
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
        claim.adopt(3, kSecond, kTwo);
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
    a.adopt(1, kFirst, kOne);
    b.adopt(1, kSecond, kTwo);
    EXPECT_EQ(held, 2U);
    a.release(1);
    EXPECT_EQ(held, 1U);
    EXPECT_TRUE(b.held_by(1));
}

// The catalog is told which grant goes back, not only which upload.
TEST(UploadClaim, AReleaseNamesTheGrantItGivesBack) {
    ReleaseLog catalog;
    std::size_t held = 0;
    UploadClaim claim(catalog, held);
    claim.adopt(1, kFirst, kOne);
    EXPECT_EQ(claim.token_of(1), kOne);
    claim.release(1);
    claim.adopt(2, kFirst, kTwo);
    EXPECT_EQ(claim.token_of(1), std::nullopt);
    EXPECT_EQ(claim.token_of(2), kTwo);
    claim.release(2);
    EXPECT_EQ(claim.token_of(2), std::nullopt);
    EXPECT_EQ(catalog.released, (std::vector{kFirst, kFirst}));
    EXPECT_EQ(catalog.tokens, (std::vector{kOne, kTwo}));
}

class MemoryCatalogClaims : public ::testing::Test {
protected:
    [[nodiscard]] core::ports::CatalogResult<void> progress(ClaimToken token,
                                                            std::uint64_t offset) {
        std::optional<core::ports::CatalogResult<void>> out;
        catalog.record_progress(id, token, video, offset,
                                [&](auto r) noexcept { out = std::move(r); });
        EXPECT_TRUE(ulw::test::pump_until(*reactor, [&] { return out.has_value(); }));
        return out.value_or(std::unexpected(CatalogError::Unavailable));
    }

    [[nodiscard]] core::ports::CatalogResult<ClaimedUpload> claim() {
        std::optional<core::ports::CatalogResult<ClaimedUpload>> out;
        catalog.claim_upload(id, owner, [&](auto r) noexcept { out = std::move(r); });
        EXPECT_TRUE(ulw::test::pump_until(*reactor, [&] { return out.has_value(); }));
        return out.value_or(std::unexpected(CatalogError::Unavailable));
    }

    os::SystemClock clock;
    os::SystemRandom random;
    std::unique_ptr<net::IReactor> reactor =
        std::move(*net::make_reactor(ulw::test::reactor_kind_from_env(), clock, 64));
    infra::catalog::MemoryCatalog catalog{*reactor, clock};
    core::UserId owner = *core::UserId::parse("alice");
    core::VideoId video = core::VideoId::generate(clock, random);
    core::UploadId id = core::UploadId::generate(clock, random);

    void SetUp() override {
        std::optional<core::ports::CatalogResult<void>> made;
        catalog.create_upload(
            core::ports::NewUpload{
                .video = core::VideoRecord{.id = video,
                                           .owner = owner,
                                           .title = "trip",
                                           .state = core::VideoState::Init,
                                           .version = 0,
                                           .error_reason = std::nullopt,
                                           .duration = std::nullopt},
                .upload =
                    core::UploadRecord{.id = id,
                                       .video_id = video,
                                       .owner = owner,
                                       .size_bytes = 1024,
                                       .chunk_size = 1024,
                                       .durable_offset = 0,
                                       .state = core::UploadState::Active,
                                       .expires_at = clock.wall_now() + std::chrono::hours(24)},
                .backend_ref = "ingest-1",
                .object_key = *core::StorageKey::parse("videos/" + video.to_string() + "/raw")},
            [&](auto r) noexcept { made = r; });
        ASSERT_TRUE(ulw::test::pump_until(*reactor, [&] { return made.has_value(); }));
        ASSERT_TRUE(*made);
    }
};

// A holder whose grant was given back, and the upload claimed again, releases nothing with its
// old token: the later grant stays held until its own token comes back.
TEST_F(MemoryCatalogClaims, AReleaseWithAnEarlierGrantsTokenReleasesNothing) {
    const auto first = claim();
    ASSERT_TRUE(first);
    catalog.release_upload(id, first->token);
    EXPECT_EQ(catalog.claims(), 0U);
    const auto second = claim();
    ASSERT_TRUE(second);
    EXPECT_NE(second->token, first->token);
    catalog.release_upload(id, first->token);
    EXPECT_EQ(catalog.claims(), 1U);
    EXPECT_EQ(claim().error(), CatalogError::Conflict);
    catalog.release_upload(id, second->token);
    EXPECT_EQ(catalog.claims(), 0U);
    EXPECT_TRUE(claim());
}

// An append still running under a grant that was given back, the upload since claimed again,
// records nothing under the new grant; the new grant's own progress is recorded.
TEST_F(MemoryCatalogClaims, ProgressUnderAnEarlierGrantsTokenIsRefused) {
    const auto first = claim();
    ASSERT_TRUE(first);
    EXPECT_TRUE(progress(first->token, 256));
    catalog.release_upload(id, first->token);
    EXPECT_EQ(progress(first->token, 512).error(), CatalogError::Conflict);
    const auto second = claim();
    ASSERT_TRUE(second);
    EXPECT_EQ(progress(first->token, 512).error(), CatalogError::Conflict);
    EXPECT_TRUE(progress(second->token, 512));
}

} // namespace
