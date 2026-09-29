// The reaper end to end: a real Postgres for the catalog and a real MinIO for the sessions, in
// a bucket of this run's own, since the sweep aborts every old session in whatever bucket it is
// pointed at.
#include "core/models/content_type.hpp"
#include "core/models/storage_key.hpp"
#include "infra/curl/multi.hpp"
#include "infra/postgres/upload_reaper.hpp"
#include "infra/storage/s3_store.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "postgres_harness.hpp"
#include "reaper.hpp"
#include "support/live_s3.hpp"
#include "support/reactor_harness.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace {

using infra::postgres::Params;
using infra::s3util::MultipartUpload;
using ulw::test::scalar;

constexpr std::chrono::hours kOrphanAfter{7 * 24};

// A wall clock that stands where the test says, so a session the server stamped a moment ago
// can be made to look as old as the sweep needs.
class FixedClock final : public core::ports::IClock {
public:
    explicit FixedClock(core::WallTime wall) noexcept : wall_(wall) {}
    [[nodiscard]] core::MonoTime now() const noexcept override { return {}; }
    [[nodiscard]] core::WallTime wall_now() const noexcept override { return wall_; }

private:
    core::WallTime wall_;
};

class NoObserver final : public core::ports::IIngestObserver {
public:
    void on_ingest_progress() noexcept override {}
};

class UploadReaperLive : public ::testing::Test {
protected:
    void SetUp() override {
        ulw::test::ScratchDatabase::open(db_);
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        if (!ulw::test::ensure_bucket(target_)) {
#ifdef ULW_CONFORMANCE_LIVE
            FAIL() << "MinIO unreachable";
#else
            GTEST_SKIP() << "MinIO unreachable; start deploy/local/compose.yaml or set "
                            "ULW_MINIO_ENDPOINT";
#endif
        }
        bucket_made_ = true;
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
        reaper_ = std::make_unique<infra::postgres::PgUploadReaper>(db_->conninfo());
    }

    void TearDown() override {
        store_.reset();
        multi_.reset();
        reactor_.reset();
        if (bucket_made_) {
            ulw::test::abort_uploads(target_, "");
            const auto bucket = infra::s3util::Bucket::make(target_.profile, target_.bucket);
            if (bucket) {
                [[maybe_unused]] const auto dropped =
                    ulw::test::send(target_, infra::curl::Method::Delete, bucket->root());
            }
        }
    }

    // A session in the store, and the catalog rows that own it when `expires_in_seconds` is
    // given: a video and an upload that expire that many seconds from now.
    std::optional<core::ports::IngestId> start(const std::string& name,
                                               std::optional<std::int64_t> expires_in_seconds) {
        const auto key = *core::StorageKey::parse("videos/" + name + "/raw");
        auto id = store_->create(key, 1, *core::ContentType::parse("video/mp4"));
        EXPECT_TRUE(id);
        if (!id) {
            return std::nullopt;
        }
        if (!expires_in_seconds) {
            return *id;
        }
        const auto video = core::VideoId::generate(clock_, random_);
        const auto upload = core::UploadId::generate(clock_, random_);
        auto conn = db_->session();
        EXPECT_TRUE(
            conn.exec("INSERT INTO videos (id, owner_id, title) VALUES ($1, 'auth0|t', 'x')",
                      Params{}.add_uuid(video.uuid())));
        EXPECT_TRUE(conn.exec(
            "INSERT INTO uploads (id, video_id, owner_id, backend_ref, object_key, chunk_size, "
            "size_bytes, expires_at) VALUES ($1, $2, 'auth0|t', $3, $4, 8388608, 1, "
            "now() + $5 * interval '1 second')",
            Params{}
                .add_uuid(upload.uuid())
                .add_uuid(video.uuid())
                .add_text(id->backend_ref)
                .add_text(id->key.view())
                .add_int(*expires_in_seconds)));
        videos_.emplace(name, video);
        return *id;
    }

    // The gateway's half of a commit that the reaper then overtakes: every byte sent and the
    // store's session completed into an object, before the catalog has heard of it.
    [[nodiscard]] bool complete_in_store(const core::ports::IngestId& id) {
        NoObserver observer;
        auto session = store_->open(id, 0, observer);
        if (!session) {
            return false;
        }
        const std::array<std::byte, 1> body{};
        const bool sent = ulw::test::pump_until(
            *reactor_, [&] { return (*session)->write(body) == body.size(); });
        (*session)->finish();
        const bool done = ulw::test::pump_until(*reactor_, [&] {
            return (*session)->state() != core::ports::IngestState::Open &&
                   (*session)->state() != core::ports::IngestState::Finalizing;
        });
        session->reset();
        return sent && done && store_->commit(id).has_value();
    }

    [[nodiscard]] bool object_exists(const core::StorageKey& key) {
        const auto keys = store_->list(key.view());
        return keys && std::ranges::find(*keys, key) != keys->end();
    }

    [[nodiscard]] std::optional<MultipartUpload> listed(const std::string& upload_id) const {
        const auto open = ulw::test::open_uploads(target_, "");
        if (!open) {
            return std::nullopt;
        }
        const auto it = std::ranges::find(*open, upload_id, &MultipartUpload::upload_id);
        return it == open->end() ? std::nullopt : std::optional(*it);
    }

    [[nodiscard]] reaper::Report run(const core::ports::IClock& clock) {
        return reaper::run_once(*reaper_, *store_, *store_, clock,
                                {.batch = 10, .orphan_after = kOrphanAfter});
    }

    [[nodiscard]] std::string video_state(const std::string& name) const {
        auto conn = db_->session();
        return scalar(conn, "SELECT state FROM videos WHERE id = $1",
                      Params{}.add_uuid(videos_.at(name).uuid()));
    }

    ulw::test::LiveS3 target_ = [] {
        auto minio = ulw::test::minio_from_env();
        minio.bucket = ulw::test::unique_prefix("reaper");
        minio.bucket.pop_back();
        return minio;
    }();
    os::SystemClock clock_;
    os::SystemRandom random_;
    std::unique_ptr<ulw::test::ScratchDatabase> db_;
    std::unique_ptr<net::IReactor> reactor_;
    std::unique_ptr<infra::curl::Multi> multi_;
    std::unique_ptr<infra::storage::S3Store> store_;
    std::unique_ptr<infra::postgres::PgUploadReaper> reaper_;
    std::map<std::string, core::VideoId> videos_;
    bool bucket_made_ = false;
};

TEST_F(UploadReaperLive, AbortsTheStoreSessionOfAnExpiredUploadAndFailsItsVideo) {
    const auto expired = start("expired", -3600);
    const auto live = start("live", 3600);
    ASSERT_TRUE(expired && live);
    ASSERT_TRUE(listed(expired->backend_ref));

    const auto report = run(clock_);

    EXPECT_TRUE(report.problems.empty());
    EXPECT_EQ(report.uploads_expired, 1U);
    EXPECT_EQ(report.parts_orphaned, 0U);
    EXPECT_FALSE(listed(expired->backend_ref));
    EXPECT_TRUE(listed(live->backend_ref));
    EXPECT_EQ(video_state("expired"), "failed");
    EXPECT_EQ(video_state("live"), "init");
}

TEST_F(UploadReaperLive, AnObjectAFinishedCommitLeftBehindTheAbortedRowIsRemoved) {
    const auto raced = start("raced", -3600);
    ASSERT_TRUE(raced);
    ASSERT_TRUE(complete_in_store(*raced));
    ASSERT_TRUE(object_exists(raced->key)) << "the interleaving was not reached";

    const auto report = run(clock_);

    EXPECT_TRUE(report.problems.empty());
    EXPECT_EQ(report.uploads_expired, 1U);
    EXPECT_FALSE(object_exists(raced->key));
    EXPECT_EQ(video_state("raced"), "failed");
}

TEST_F(UploadReaperLive, ASecondPassFindsNothingLeftToDo) {
    ASSERT_TRUE(start("expired", -3600));
    ASSERT_EQ(run(clock_).uploads_expired, 1U);
    const auto again = run(clock_);
    EXPECT_TRUE(again.problems.empty());
    EXPECT_EQ(again.uploads_expired, 0U);
    EXPECT_EQ(again.parts_orphaned, 0U);
}

TEST_F(UploadReaperLive, SweepsASessionNoUploadOwnsAndKeepsOneThatIsStillLive) {
    const auto orphan = start("orphan", std::nullopt);
    ASSERT_TRUE(orphan);
    const auto orphan_seen = listed(orphan->backend_ref);
    ASSERT_TRUE(orphan_seen);
    // The cutoff is a time the server recorded, so the two clocks never need to agree. MinIO
    // stamps sessions to the millisecond; a second start inside the same one is tried again.
    std::optional<MultipartUpload> recent;
    for (int attempt = 0; attempt < 100 && (!recent || recent->initiated <= orphan_seen->initiated);
         ++attempt) {
        const auto live = start("live-" + std::to_string(attempt), 30LL * 24 * 3600);
        ASSERT_TRUE(live);
        recent = listed(live->backend_ref);
    }
    ASSERT_TRUE(recent);
    ASSERT_GT(recent->initiated, orphan_seen->initiated);

    // Seven days after the newer session began, the older one is past the bound and the newer
    // is exactly at it.
    const FixedClock later(recent->initiated + kOrphanAfter);
    const auto report = run(later);

    EXPECT_TRUE(report.problems.empty());
    EXPECT_EQ(report.parts_orphaned, 1U);
    EXPECT_FALSE(listed(orphan->backend_ref));
    EXPECT_TRUE(listed(recent->upload_id));
}

} // namespace
