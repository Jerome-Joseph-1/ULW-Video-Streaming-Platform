#include "storage_harness.hpp"

#include <gtest/gtest.h>

namespace {

using core::ports::IngestState;
using core::ports::StorageError;
using ulw::test::kLocalChunk;

class FakeStoreFaults : public ::testing::Test {
protected:
    ulw::test::FakeHarness h;
    const core::StorageKey key = *core::StorageKey::parse("videos/faults/raw");

    [[nodiscard]] core::ports::IngestId create(std::uint64_t total) {
        return *h.ingest().create(key, total, *core::ContentType::parse("video/mp4"));
    }
};

TEST_F(FakeStoreFaults, ThrottleIsRetriedAndSucceeds) {
    h.fake().set_plan({.throttle_first = 3});
    const auto data = ulw::test::pattern(2 * kLocalChunk);
    const auto id = create(data.size());
    ulw::test::Observer obs;
    auto session = h.ingest().open(id, 0, obs);
    ASSERT_TRUE(session);
    ASSERT_TRUE(h.write_all(**session, obs, data));
    ASSERT_TRUE(h.finish(**session, obs));
    EXPECT_FALSE((*session)->error().has_value());
    EXPECT_EQ((*session)->durable_offset(), data.size());
    // Two chunks, three of the attempts refused and retried.
    EXPECT_EQ(h.fake().chunk_attempts(), 5U);
    ASSERT_TRUE(h.ingest().commit(id));
    EXPECT_TRUE(*h.reader().fetch_small(key, data.size()) == data);
}

TEST_F(FakeStoreFaults, PermanentErrorDoesNotAdvanceDurableOffset) {
    h.fake().set_plan({.fail_chunk = 2, .fail_error = StorageError::Permanent});
    const auto data = ulw::test::pattern(3 * kLocalChunk);
    const auto id = create(data.size());
    ulw::test::Observer obs;
    auto session = h.ingest().open(id, 0, obs);
    ASSERT_TRUE(session);
    // Every byte may be accepted before the failed chunk reports back; the failure surfaces
    // by the time the session is finished.
    static_cast<void>(h.write_all(**session, obs, data));
    EXPECT_FALSE(h.finish(**session, obs));
    EXPECT_EQ((*session)->state(), IngestState::Failed);
    EXPECT_EQ((*session)->error(), StorageError::Permanent);
    EXPECT_EQ((*session)->durable_offset(), kLocalChunk);
    EXPECT_EQ(h.ingest().durable_offset(id), kLocalChunk);
    EXPECT_EQ(h.ingest().commit(id), std::unexpected(StorageError::PreconditionFailed));
}

TEST_F(FakeStoreFaults, StalledBackendTakesNothingAndStaysQuiet) {
    h.fake().set_plan({.accept_zero = true});
    const auto data = ulw::test::pattern(kLocalChunk);
    const auto id = create(data.size());
    ulw::test::Observer obs;
    auto session = h.ingest().open(id, 0, obs);
    ASSERT_TRUE(session);
    EXPECT_EQ((*session)->write(data), 0U);
    h.settle(obs);
    EXPECT_EQ(obs.calls, 0);
    EXPECT_EQ((*session)->state(), IngestState::Open);
    EXPECT_FALSE((*session)->error().has_value());
}

TEST_F(FakeStoreFaults, CappedWritesWakeTheWriterUntilEverythingIsTaken) {
    h.fake().set_plan({.accept_per_call = 100});
    const auto data = ulw::test::pattern(8192, 3);
    const auto id = create(data.size());
    ulw::test::Observer obs;
    auto session = h.ingest().open(id, 0, obs);
    ASSERT_TRUE(session);
    EXPECT_EQ((*session)->write(data), 100U);
    ASSERT_TRUE(h.write_all(**session, obs, std::span(data).subspan(100)));
    ASSERT_TRUE(h.finish(**session, obs));
    ASSERT_TRUE(h.ingest().commit(id));
    EXPECT_TRUE(*h.reader().fetch_small(key, data.size()) == data);
}

TEST_F(FakeStoreFaults, OpeningPastTheDurableOffsetIsRefused) {
    const auto id = create(3 * kLocalChunk);
    ulw::test::Observer obs;
    EXPECT_EQ(h.ingest().open(id, kLocalChunk, obs).error(), StorageError::PreconditionFailed);
    EXPECT_EQ(h.ingest().open(id, 10, obs).error(), StorageError::PreconditionFailed);
}

} // namespace
