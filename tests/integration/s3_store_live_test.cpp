// The S3 store's bucket-wide operations against a real MinIO, where a scripted peer would only
// echo what the test assumed.
#include "core/models/content_type.hpp"
#include "core/models/storage_key.hpp"
#include "infra/curl/multi.hpp"
#include "infra/storage/s3_store.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "support/live_s3.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>

namespace {

using infra::s3util::MultipartUpload;

class S3StoreLive : public ::testing::Test {
protected:
    void SetUp() override {
        if (!ulw::test::ensure_bucket(target_)) {
#ifdef ULW_CONFORMANCE_LIVE
            FAIL() << "MinIO unreachable";
#else
            GTEST_SKIP() << "MinIO unreachable; start deploy/local/compose.yaml or set "
                            "ULW_MINIO_ENDPOINT";
#endif
        }
        reactor_ = std::move(*net::make_reactor(net::ReactorKind::Epoll, clock_, 1024));
        multi_ = std::move(*infra::curl::Multi::create(*reactor_));
        store_ = std::move(*infra::storage::S3Store::create(
            infra::storage::S3Store::Deps{.reactor = *reactor_,
                                          .multi = *multi_,
                                          .credentials = target_.credentials,
                                          .clock = clock_,
                                          .random = random_,
                                          .profile = target_.profile,
                                          .bucket = target_.bucket}));
    }
    void TearDown() override {
        ulw::test::abort_uploads(target_, prefix_);
        store_.reset();
        multi_.reset();
        reactor_.reset();
    }

    [[nodiscard]] std::optional<MultipartUpload> start(const std::string& name) {
        const auto key = *core::StorageKey::parse(prefix_ + name);
        const auto id = store_->create(key, 1, *core::ContentType::parse("video/mp4"));
        if (!id) {
            return std::nullopt;
        }
        return listed(id->backend_ref);
    }

    [[nodiscard]] std::optional<MultipartUpload> listed(const std::string& upload_id) const {
        const auto open = ulw::test::open_uploads(target_, prefix_);
        if (!open) {
            return std::nullopt;
        }
        const auto it = std::ranges::find(*open, upload_id, &MultipartUpload::upload_id);
        return it == open->end() ? std::nullopt : std::optional(*it);
    }

    ulw::test::LiveS3 target_ = ulw::test::minio_from_env();
    std::string prefix_ = ulw::test::unique_prefix("reap");
    os::SystemClock clock_;
    os::SystemRandom random_;
    std::unique_ptr<net::IReactor> reactor_;
    std::unique_ptr<infra::curl::Multi> multi_;
    std::unique_ptr<infra::storage::S3Store> store_;
};

// The reaper sweeps the whole bucket, so this test aborts any upload another run started before
// its cutoff: it relies on the integration label running one test at a time.
TEST_F(S3StoreLive, ReapingAbortsUploadsStartedBeforeTheCutoffAndKeepsLaterOnes) {
    const auto abandoned = start("abandoned");
    ASSERT_TRUE(abandoned);
    // The cutoff is a time the server recorded, so the two clocks never need to agree. MinIO
    // stamps uploads to the millisecond; a second start inside the same one is tried again.
    std::optional<MultipartUpload> recent;
    for (int attempt = 0; attempt < 100 && (!recent || recent->initiated <= abandoned->initiated);
         ++attempt) {
        recent = start("recent-" + std::to_string(attempt));
    }
    ASSERT_TRUE(recent);
    ASSERT_GT(recent->initiated, abandoned->initiated);

    const auto reaped = store_->reap_abandoned(recent->initiated);
    ASSERT_TRUE(reaped);
    EXPECT_GE(*reaped, 1U);
    EXPECT_FALSE(listed(abandoned->upload_id));
    EXPECT_TRUE(listed(recent->upload_id));
}

} // namespace
