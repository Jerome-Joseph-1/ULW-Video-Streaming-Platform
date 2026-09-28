// Laws every IIngestStore must obey, run against each backend. Fake and fs always run;
// live backends join when ULW_CONFORMANCE_LIVE is set at configure time.
#include "storage_harness.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <string>

namespace {

using core::ports::IngestId;
using core::ports::IngestState;
using core::ports::StorageError;
using ulw::test::StorageHarness;

class StoreConformance : public ::testing::TestWithParam<ulw::test::StoreFactory> {
protected:
    void SetUp() override {
        harness = GetParam().make();
        ASSERT_NE(harness, nullptr) << "backend unavailable";
        chunk = harness->ingest().preferred_chunk_size();
    }

    [[nodiscard]] core::StorageKey key(std::string_view suffix) const {
        return *core::StorageKey::parse(harness->key_prefix() + std::string(suffix));
    }

    [[nodiscard]] IngestId create(std::string_view suffix, std::uint64_t total) {
        auto id =
            harness->ingest().create(key(suffix), total, *core::ContentType::parse("video/mp4"));
        EXPECT_TRUE(id.has_value());
        return *id;
    }

    // Opens at `from`, writes data[from, to) and finishes.
    void send(const IngestId& id, std::span<const std::byte> data, std::uint64_t from,
              std::uint64_t to) {
        ulw::test::Observer obs;
        auto session = harness->ingest().open(id, from, obs);
        ASSERT_TRUE(session);
        ASSERT_TRUE(harness->write_all(**session, obs, data.subspan(from, to - from)));
        ASSERT_TRUE(harness->finish(**session, obs));
    }

    // Opens at `from`, writes `count` bytes, aborts, and lets the backend finish whatever it
    // had already accepted.
    void send_and_abort(const IngestId& id, std::span<const std::byte> data, std::uint64_t from,
                        std::uint64_t count) {
        ulw::test::Observer obs;
        auto session = harness->ingest().open(id, from, obs);
        ASSERT_TRUE(session);
        ASSERT_TRUE(harness->write_all(**session, obs, data.subspan(from, count)));
        (*session)->abort();
        harness->settle(obs);
    }

    // Resumes wherever the store says, commits, and reads the object back.
    void finish_and_compare(const IngestId& id, std::span<const std::byte> data) {
        const auto at = harness->ingest().durable_offset(id);
        ASSERT_TRUE(at);
        ASSERT_NO_FATAL_FAILURE(send(id, data, *at, data.size()));
        ASSERT_TRUE(harness->ingest().commit(id));
        const auto stored = harness->reader().fetch_small(id.key, data.size());
        ASSERT_TRUE(stored);
        EXPECT_TRUE(std::ranges::equal(*stored, data));
    }

    std::unique_ptr<StorageHarness> harness;
    std::uint64_t chunk = 0;
};

TEST_P(StoreConformance, DurableOffsetStartsAtZero) {
    const IngestId id = create("zero", 2 * chunk);
    EXPECT_EQ(harness->ingest().durable_offset(id), 0U);
    ulw::test::Observer obs;
    auto session = harness->ingest().open(id, 0, obs);
    ASSERT_TRUE(session);
    EXPECT_EQ((*session)->durable_offset(), 0U);
    EXPECT_EQ((*session)->state(), IngestState::Open);
}

TEST_P(StoreConformance, DurableOffsetIsMonotonic) {
    const auto data = ulw::test::pattern(3 * chunk);
    const IngestId id = create("monotonic", data.size());
    std::uint64_t last = 0;
    for (std::uint64_t i = 0; i < 3; ++i) {
        ulw::test::Observer obs;
        auto session = harness->ingest().open(id, i * chunk, obs);
        ASSERT_TRUE(session);
        obs.on_progress = [&] {
            const std::uint64_t now = (*session)->durable_offset();
            EXPECT_GE(now, last);
            last = now;
        };
        ASSERT_TRUE(harness->write_all(**session, obs, std::span(data).subspan(i * chunk, chunk)));
        ASSERT_TRUE(harness->finish(**session, obs));
        EXPECT_EQ((*session)->durable_offset(), (i + 1) * chunk);
        EXPECT_EQ(harness->ingest().durable_offset(id), (i + 1) * chunk);
    }
}

TEST_P(StoreConformance, DurableOffsetIsALegalResumePoint) {
    const auto data = ulw::test::pattern((2 * chunk) + (chunk / 2), 17);
    const IngestId id = create("resume", data.size());
    {
        ulw::test::Observer obs;
        auto session = harness->ingest().open(id, 0, obs);
        ASSERT_TRUE(session);
        ASSERT_TRUE(harness->write_all(**session, obs, std::span(data).first(chunk + (chunk / 2))));
        // A whole chunk the backend accepted becomes durable without finish(): that is what
        // lets a cut-off upload resume past zero. Wait for exactly that, then cut it off.
        ASSERT_TRUE(ulw::test::pump_until(
            harness->reactor(), [&] { return (*session)->durable_offset() >= chunk; },
            ulw::test::kOperationLimit));
        (*session)->abort();
    }
    const auto resume_at = harness->ingest().durable_offset(id);
    ASSERT_TRUE(resume_at);
    EXPECT_GE(*resume_at, chunk);
    EXPECT_LE(*resume_at, chunk + (chunk / 2));

    ulw::test::Observer obs;
    auto session = harness->ingest().open(id, *resume_at, obs);
    ASSERT_TRUE(session);
    ASSERT_TRUE(harness->write_all(**session, obs, std::span(data).subspan(*resume_at)));
    ASSERT_TRUE(harness->finish(**session, obs));
    ASSERT_TRUE(harness->ingest().commit(id));
    const auto stored = harness->reader().fetch_small(id.key, data.size());
    ASSERT_TRUE(stored);
    EXPECT_TRUE(*stored == data);
}

// A caller's cached offset may lag the store's, so reopening below the durable offset is
// legal and must only ever re-send bytes the store already holds.
TEST_P(StoreConformance, StaleLowReopenKeepsTheBytesAlreadyDurable) {
    const auto data = ulw::test::pattern(3 * chunk, 23);
    const IngestId id = create("stale-short", data.size());
    ASSERT_NO_FATAL_FAILURE(send(id, data, 0, 2 * chunk));
    ASSERT_NO_FATAL_FAILURE(send_and_abort(id, data, 0, chunk / 2));
    EXPECT_EQ(harness->ingest().durable_offset(id), 2 * chunk);
    ASSERT_NO_FATAL_FAILURE(finish_and_compare(id, data));
}

TEST_P(StoreConformance, StaleLowReopenNeverLowersTheDurableOffset) {
    const auto data = ulw::test::pattern(3 * chunk, 29);
    const IngestId id = create("stale-long", data.size());
    ASSERT_NO_FATAL_FAILURE(send(id, data, 0, 2 * chunk));
    // Past a chunk boundary, where a backend makes what it received durable.
    ASSERT_NO_FATAL_FAILURE(send_and_abort(id, data, 0, chunk + (chunk / 2)));
    EXPECT_EQ(harness->ingest().durable_offset(id), 2 * chunk);
    ASSERT_NO_FATAL_FAILURE(finish_and_compare(id, data));
}

TEST_P(StoreConformance, SessionOpenedPastTheDurableOffsetFails) {
    const auto data = ulw::test::pattern(3 * chunk, 31);
    const IngestId id = create("past-durable", data.size());
    ulw::test::Observer obs;
    auto session = harness->ingest().open(id, chunk, obs);
    ASSERT_TRUE(session) << "open() cannot tell without blocking; the session must";
    // Taking the bytes and failing afterwards is as legal as refusing them.
    static_cast<void>(harness->write_all(**session, obs, std::span(data).subspan(chunk, chunk)));
    // Failed, and the observer told so: a writer that got 0 back waits for that call.
    ASSERT_TRUE(ulw::test::pump_until(
        harness->reactor(),
        [&] { return (*session)->state() == IngestState::Failed && obs.calls > 0; },
        ulw::test::kOperationLimit));
    EXPECT_EQ((*session)->error(), StorageError::PreconditionFailed);
    session->reset();
    EXPECT_EQ(harness->ingest().durable_offset(id), 0U);
    EXPECT_EQ(harness->ingest().commit(id), std::unexpected(StorageError::PreconditionFailed));
    ASSERT_NO_FATAL_FAILURE(finish_and_compare(id, data));
}

TEST_P(StoreConformance, AbortIsIdempotentAndSafe) {
    const auto data = ulw::test::pattern(2 * chunk);
    const IngestId id = create("abort", data.size());
    ulw::test::Observer obs;
    auto session = harness->ingest().open(id, 0, obs);
    ASSERT_TRUE(session);
    // A whole chunk, so the backend has work of its own in flight when the abort lands.
    ASSERT_TRUE(harness->write_all(**session, obs, std::span(data).first(chunk + (chunk / 2))));
    (*session)->abort();
    (*session)->abort();
    const int calls = obs.calls;
    harness->settle(obs);
    EXPECT_EQ(obs.calls, calls) << "observer called after abort";
    session->reset();

    const auto at = harness->ingest().durable_offset(id);
    ASSERT_TRUE(at);
    ulw::test::Observer again;
    auto reopened = harness->ingest().open(id, *at, again);
    ASSERT_TRUE(reopened);
    ASSERT_TRUE(harness->write_all(**reopened, again, std::span(data).subspan(*at)));
    ASSERT_TRUE(harness->finish(**reopened, again));
    ASSERT_TRUE(harness->ingest().commit(id));
    const auto stored = harness->reader().fetch_small(id.key, data.size());
    ASSERT_TRUE(stored);
    EXPECT_TRUE(*stored == data);
}

TEST_P(StoreConformance, CommitIsIdempotent) {
    const auto data = ulw::test::pattern(chunk + 100);
    const IngestId id = ulw::test::upload_whole(*harness, key("idempotent"), data);
    ASSERT_TRUE(harness->ingest().commit(id));
    ASSERT_TRUE(harness->ingest().commit(id));
    const auto stored = harness->reader().fetch_small(id.key, data.size());
    ASSERT_TRUE(stored);
    EXPECT_TRUE(*stored == data);
}

TEST_P(StoreConformance, ObjectEndingInAShortChunkCommitsWhole) {
    const auto data = ulw::test::pattern((2 * chunk) + (chunk / 2), 5);
    const IngestId id = ulw::test::upload_whole(*harness, key("short-tail"), data);
    EXPECT_EQ(harness->ingest().durable_offset(id), data.size());
    ASSERT_TRUE(harness->ingest().commit(id));
    const auto stored = harness->reader().fetch_small(id.key, data.size());
    ASSERT_TRUE(stored);
    EXPECT_TRUE(*stored == data);
}

TEST_P(StoreConformance, WriteReportsBackpressureNotFailure) {
    const auto data = ulw::test::pattern(4 * chunk);
    const IngestId id = create("backpressure", data.size());
    ulw::test::Observer obs;
    auto session = harness->ingest().open(id, 0, obs);
    ASSERT_TRUE(session);
    // Without turning the loop nothing can drain, so a backend must say "not now" long before
    // the whole object is buffered, and must not call that an error.
    std::size_t accepted = 0;
    for (int i = 0; i < 1000 && accepted < data.size(); ++i) {
        const std::size_t n = (*session)->write(std::span(data).subspan(accepted));
        if (n == 0) {
            break;
        }
        accepted += n;
    }
    EXPECT_LT(accepted, data.size());
    EXPECT_EQ((*session)->write(std::span(data).subspan(accepted)), 0U);
    EXPECT_EQ((*session)->state(), IngestState::Open);
    EXPECT_FALSE((*session)->error().has_value());
    EXPECT_FALSE((*session)->wants_more());

    ASSERT_TRUE(harness->write_all(**session, obs, std::span(data).subspan(accepted)));
    ASSERT_TRUE(harness->finish(**session, obs));
    EXPECT_EQ((*session)->durable_offset(), data.size());
}

TEST_P(StoreConformance, CommitBeforeAllBytesFails) {
    const auto data = ulw::test::pattern(2 * chunk);
    const IngestId id = create("early", data.size());
    ulw::test::Observer obs;
    auto session = harness->ingest().open(id, 0, obs);
    ASSERT_TRUE(session);
    ASSERT_TRUE(harness->write_all(**session, obs, std::span(data).first(chunk)));
    ASSERT_TRUE(harness->finish(**session, obs));
    EXPECT_EQ(harness->ingest().commit(id), std::unexpected(StorageError::PreconditionFailed));
    EXPECT_EQ(harness->reader().fetch_small(id.key, data.size()),
              std::unexpected(StorageError::NotFound));
}

TEST_P(StoreConformance, DiscardMakesTheObjectUnreadable) {
    const auto data = ulw::test::pattern(chunk + 1);
    const IngestId id = create("discard", data.size());
    ulw::test::Observer obs;
    auto session = harness->ingest().open(id, 0, obs);
    ASSERT_TRUE(session);
    ASSERT_TRUE(harness->write_all(**session, obs, data));
    ASSERT_TRUE(harness->finish(**session, obs));
    session->reset();
    harness->ingest().discard(id);
    harness->ingest().discard(id);
    EXPECT_FALSE(harness->ingest().commit(id).has_value());
    EXPECT_EQ(harness->reader().fetch_small(id.key, data.size()),
              std::unexpected(StorageError::NotFound));
}

TEST_P(StoreConformance, OpenRefusesAnIngestWithoutAChunkSize) {
    IngestId id = create("no-chunk", 2 * chunk);
    // The id comes back from the database; a damaged row must not reach a division.
    id.chunk_size = 0;
    ulw::test::Observer obs;
    EXPECT_FALSE(harness->ingest().open(id, 0, obs).has_value());
}

TEST_P(StoreConformance, KeysRoundTripExactly) {
    for (const std::string_view k : {"a/b/c", "video-01J8/raw", "a.b_c-d/1"}) {
        const auto data = ulw::test::pattern(1000, k.size());
        const IngestId id = ulw::test::upload_whole(*harness, key(k), data);
        ASSERT_TRUE(harness->ingest().commit(id)) << k;
        EXPECT_EQ(id.key, key(k));
        const auto stored = harness->reader().fetch_small(key(k), data.size());
        ASSERT_TRUE(stored) << k;
        EXPECT_TRUE(*stored == data) << k;
        const auto listed = harness->admin().list(key(k).view());
        ASSERT_TRUE(listed) << k;
        EXPECT_TRUE(std::ranges::find(*listed, key(k)) != listed->end()) << k;
    }
}

TEST_P(StoreConformance, KeysThatLookLikeTemporaryFilesAreOrdinaryKeys) {
    const auto staged = ulw::test::pattern(100, 1);
    const auto plain = ulw::test::pattern(200, 2);
    ASSERT_TRUE(harness->admin().put(key("scratch/x.tmp"), staged));
    ASSERT_TRUE(harness->admin().put(key("scratch/x"), plain));
    const auto first = harness->reader().fetch_small(key("scratch/x.tmp"), staged.size());
    ASSERT_TRUE(first) << "writing x disturbed x.tmp";
    EXPECT_TRUE(*first == staged);
    const auto second = harness->reader().fetch_small(key("scratch/x"), plain.size());
    ASSERT_TRUE(second);
    EXPECT_TRUE(*second == plain);
    const auto listed = harness->admin().list(harness->key_prefix() + "scratch/");
    ASSERT_TRUE(listed);
    EXPECT_TRUE(std::ranges::find(*listed, key("scratch/x.tmp")) != listed->end());
    EXPECT_TRUE(std::ranges::find(*listed, key("scratch/x")) != listed->end());
}

INSTANTIATE_TEST_SUITE_P(Backends, StoreConformance,
                         ::testing::ValuesIn(ulw::test::store_factories()),
                         [](const ::testing::TestParamInfo<ulw::test::StoreFactory>& p) {
                             return p.param.name;
                         });

} // namespace
