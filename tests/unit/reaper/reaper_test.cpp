#include "core/models/content_type.hpp"

#include "reaper.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"

#include <deque>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

using core::ports::CatalogError;
using core::ports::ExpiredUpload;
using core::ports::StorageError;

class FakeCatalog final : public core::ports::IUploadExpiry {
public:
    [[nodiscard]] std::expected<std::vector<ExpiredUpload>, CatalogError>
    expire(core::WallTime now, std::size_t limit) override {
        calls.push_back({now, limit});
        if (replies.empty()) {
            return std::vector<ExpiredUpload>{};
        }
        auto reply = std::move(replies.front());
        replies.pop_front();
        return reply;
    }

    struct Call {
        core::WallTime now;
        std::size_t limit;
    };
    std::vector<Call> calls;
    std::deque<std::expected<std::vector<ExpiredUpload>, CatalogError>> replies;
};

class FakeStore final : public core::ports::IIngestStore, public core::ports::IObjectAdmin {
public:
    [[nodiscard]] std::expected<core::ports::IngestId, StorageError>
    create(const core::StorageKey& /*key*/, std::uint64_t /*total*/,
           const core::ContentType& /*type*/) override {
        return std::unexpected(StorageError::Permanent);
    }
    [[nodiscard]] std::expected<std::unique_ptr<core::ports::IIngestSession>, StorageError>
    open(const core::ports::IngestId& /*id*/, std::uint64_t /*offset*/,
         core::ports::IIngestObserver& /*observer*/) override {
        return std::unexpected(StorageError::Permanent);
    }
    [[nodiscard]] std::expected<std::uint64_t, StorageError>
    durable_offset(const core::ports::IngestId& /*id*/) override {
        return std::unexpected(StorageError::Permanent);
    }
    [[nodiscard]] std::expected<void, StorageError>
    commit(const core::ports::IngestId& /*id*/) override {
        return std::unexpected(StorageError::Permanent);
    }
    void discard(const core::ports::IngestId& id) noexcept override {
        discarded.push_back(id.backend_ref);
    }
    [[nodiscard]] std::uint64_t preferred_chunk_size() const noexcept override { return 1; }

    [[nodiscard]] std::expected<void, StorageError>
    put(const core::StorageKey& /*key*/, std::span<const std::byte> /*bytes*/) override {
        return std::unexpected(StorageError::Permanent);
    }
    [[nodiscard]] std::expected<void, StorageError>
    remove(const core::StorageKey& /*key*/) override {
        return std::unexpected(StorageError::Permanent);
    }
    [[nodiscard]] std::expected<std::vector<core::StorageKey>, StorageError>
    list(std::string_view /*prefix*/) override {
        return std::unexpected(StorageError::Permanent);
    }
    [[nodiscard]] std::expected<std::size_t, StorageError>
    reap_abandoned(core::WallTime older_than) override {
        cutoffs.push_back(older_than);
        return sweep;
    }

    std::vector<std::string> discarded;
    std::vector<core::WallTime> cutoffs;
    std::expected<std::size_t, StorageError> sweep = 0;
};

class ReaperTest : public ::testing::Test {
protected:
    [[nodiscard]] std::vector<ExpiredUpload> uploads(std::size_t n) {
        std::vector<ExpiredUpload> out;
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back({.id = core::UploadId::generate(clock, random),
                           .ingest = {.key = *core::StorageKey::parse("videos/v/original"),
                                      .backend_ref = "session-" + std::to_string(next_++),
                                      .total_bytes = 1,
                                      .chunk_size = 1}});
        }
        return out;
    }

    [[nodiscard]] reaper::Report run(std::size_t batch = 3) {
        return reaper::run_once(catalog, store, store, clock,
                                {.batch = batch, .orphan_after = std::chrono::hours(7 * 24)});
    }

    ulw::test::FakeClock clock;
    ulw::test::FakeRandom random;
    FakeCatalog catalog;
    FakeStore store;

private:
    int next_ = 0;
};

TEST_F(ReaperTest, ReleasesTheSessionOfEveryUploadTheCatalogExpired) {
    catalog.replies.emplace_back(uploads(2));
    const auto report = run();
    EXPECT_EQ(report.uploads_expired, 2U);
    EXPECT_EQ(store.discarded, (std::vector<std::string>{"session-0", "session-1"}));
    EXPECT_TRUE(report.problems.empty());
}

TEST_F(ReaperTest, AsksAgainOnlyWhileBatchesComeBackFull) {
    catalog.replies.emplace_back(uploads(3));
    catalog.replies.emplace_back(uploads(3));
    catalog.replies.emplace_back(uploads(1));
    const auto report = run(3);
    EXPECT_EQ(report.uploads_expired, 7U);
    EXPECT_EQ(catalog.calls.size(), 3U);
    for (const auto& call : catalog.calls) {
        EXPECT_EQ(call.limit, 3U);
    }
}

TEST_F(ReaperTest, ExpiresAsOfTheWallClock) {
    static_cast<void>(run());
    ASSERT_EQ(catalog.calls.size(), 1U);
    EXPECT_EQ(catalog.calls[0].now, clock.wall_now());
}

TEST_F(ReaperTest, SweepsOnlySessionsOlderThanAnyUploadMayBe) {
    static_cast<void>(run());
    ASSERT_EQ(store.cutoffs.size(), 1U);
    EXPECT_EQ(store.cutoffs[0], clock.wall_now() - std::chrono::hours(7 * 24));
}

TEST_F(ReaperTest, CountsTheSessionsTheSweepAborted) {
    store.sweep = 4;
    const auto report = run();
    EXPECT_EQ(report.parts_orphaned, 4U);
    EXPECT_EQ(reaper::metrics_text(report),
              "# TYPE uploads_expired_total counter\nuploads_expired_total 0\n"
              "# TYPE parts_orphaned_total counter\nparts_orphaned_total 4\n");
}

TEST_F(ReaperTest, ACatalogFailureIsReportedAndTheSweepStillRuns) {
    catalog.replies.emplace_back(std::unexpected(CatalogError::Unavailable));
    store.sweep = 2;
    const auto report = run();
    ASSERT_EQ(report.problems.size(), 1U);
    EXPECT_NE(report.problems[0].find("expire uploads"), std::string::npos);
    EXPECT_EQ(report.parts_orphaned, 2U);
    EXPECT_EQ(report.uploads_expired, 0U);
}

TEST_F(ReaperTest, AFailedSweepIsReportedAndTheExpiredUploadsStillCount) {
    catalog.replies.emplace_back(uploads(1));
    store.sweep = std::unexpected(StorageError::Transient);
    const auto report = run();
    ASSERT_EQ(report.problems.size(), 1U);
    EXPECT_NE(report.problems[0].find("sweep"), std::string::npos);
    EXPECT_EQ(report.uploads_expired, 1U);
    EXPECT_EQ(report.parts_orphaned, 0U);
}

TEST_F(ReaperTest, UploadsAbortedBeforeAFailureKeepTheirReleasedSessions) {
    catalog.replies.emplace_back(uploads(3));
    catalog.replies.emplace_back(std::unexpected(CatalogError::Unavailable));
    const auto report = run(3);
    EXPECT_EQ(report.uploads_expired, 3U);
    EXPECT_EQ(store.discarded.size(), 3U);
    EXPECT_EQ(report.problems.size(), 1U);
}

} // namespace
