#include "infra/storage/fs_store.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"

#include "storage_harness.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <latch>
#include <span>
#include <stdexcept>

namespace {

using infra::storage::FsStore;

class FsStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string tmpl = (std::filesystem::temp_directory_path() / "ulw-fs-XXXXXX").string();
        ASSERT_NE(::mkdtemp(tmpl.data()), nullptr);
        root = tmpl;
        auto r = net::make_reactor(ulw::test::reactor_kind_from_env(), clock, 1024);
        ASSERT_TRUE(r);
        reactor = std::move(*r);
    }
    void TearDown() override {
        reactor.reset();
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    [[nodiscard]] FsStore::Deps deps() { return {.clock = clock, .random = random}; }

    // One thread by default, so jobs run in the order they were submitted.
    [[nodiscard]] std::unique_ptr<net::OffloadPool> writer(std::size_t threads = 1) {
        auto p = net::OffloadPool::create(*reactor, threads);
        EXPECT_TRUE(p);
        return std::move(*p);
    }

    [[nodiscard]] FsStore make_store() { return {deps(), writer(), root, ulw::test::kLocalChunk}; }

    [[nodiscard]] core::ports::IngestId create(FsStore& store, std::uint64_t total) {
        auto id = store.create(key, total, *core::ContentType::parse("video/mp4"));
        EXPECT_TRUE(id);
        return *id;
    }

    // Writes every byte and finishes, turning the loop whenever the store pushes back.
    [[nodiscard]] bool upload(core::ports::IIngestSession& session, ulw::test::Observer& obs,
                              std::span<const std::byte> data) {
        while (!data.empty()) {
            const std::size_t n = session.write(data);
            data = data.subspan(n);
            const int before = obs.calls;
            if (n == 0 && !ulw::test::pump_until(*reactor, [&] { return obs.calls != before; })) {
                return false;
            }
        }
        session.finish();
        return ulw::test::pump_until(
                   *reactor,
                   [&] { return session.state() != core::ports::IngestState::Finalizing; }) &&
               session.state() == core::ports::IngestState::Committed;
    }

    [[nodiscard]] std::filesystem::path ingest(const core::ports::IngestId& id) const {
        return root / "ingest" / id.backend_ref;
    }

    const core::StorageKey key = *core::StorageKey::parse("videos/fs/raw");
    ulw::test::FakeClock clock;
    ulw::test::FakeRandom random;
    std::filesystem::path root;
    std::unique_ptr<net::IReactor> reactor;
};

// Runs after everything submitted before it on a one-thread pool.
struct Marker final : net::IOffloadJob {
    std::latch ran{1};
    void run() noexcept override { ran.count_down(); }
    void complete() noexcept override {}
};

// Holds a pool thread until the test lets it go.
struct Gate final : net::IOffloadJob {
    std::latch started{1};
    std::latch release{1};
    void run() noexcept override {
        started.count_down();
        release.wait();
    }
    void complete() noexcept override {}
};

TEST_F(FsStoreTest, StoreCanGoWhileAWriteAwaitsCompletion) {
    auto pool = writer();
    net::OffloadPool& borrowed = *pool;
    auto store = std::make_unique<FsStore>(deps(), std::move(pool), root, ulw::test::kLocalChunk);
    const auto id = create(*store, 2 * ulw::test::kLocalChunk);
    ulw::test::Observer obs;
    auto session = store->open(id, 0, obs);
    ASSERT_TRUE(session);
    const auto data = ulw::test::pattern(ulw::test::kLocalChunk);
    ASSERT_EQ((*session)->write(data), data.size());
    Marker after;
    borrowed.submit(after);
    after.ran.wait();
    // The write has run and its completion is waiting on the loop for a store that is gone.
    session->reset();
    store.reset();
    for (int turn = 0; turn < 3; ++turn) {
        static_cast<void>(reactor->run_once(core::Millis{0}));
    }
    FsStore reopened = make_store();
    EXPECT_EQ(reopened.durable_offset(id), ulw::test::kLocalChunk);
    EXPECT_EQ(obs.calls, 0);
}

TEST_F(FsStoreTest, ZeroChunkSizeIsAProgrammingError) {
    EXPECT_THROW(FsStore(deps(), writer(), root, 0), std::invalid_argument);
    EXPECT_THROW(FsStore(deps(), nullptr, root, ulw::test::kLocalChunk), std::invalid_argument);
}

TEST_F(FsStoreTest, CommitRefusesADataFileShorterThanTheObject) {
    FsStore store = make_store();
    const auto data = ulw::test::pattern(2 * ulw::test::kLocalChunk);
    const auto id = create(store, data.size());
    ulw::test::Observer obs;
    auto session = store.open(id, 0, obs);
    ASSERT_TRUE(session);
    ASSERT_TRUE(upload(**session, obs, data));
    // What a crash that lost the tail of the file, or the file itself, leaves behind: the
    // durable offset says every byte is there and the data file disagrees.
    std::filesystem::resize_file(ingest(id) / "data", ulw::test::kLocalChunk);
    EXPECT_EQ(store.commit(id), std::unexpected(core::ports::StorageError::Corrupt));
    EXPECT_EQ(store.fetch_small(key, data.size()),
              std::unexpected(core::ports::StorageError::NotFound));
}

TEST_F(FsStoreTest, UnreadableMarkerIsAnErrorNotAnException) {
    FsStore store = make_store();
    const auto id = create(store, 100);
    // A symlink to itself: stat() fails with ELOOP even for root, which a permission bit
    // would not stop.
    std::filesystem::create_symlink("committed", ingest(id) / "committed");
    EXPECT_FALSE(store.durable_offset(id).has_value());
    EXPECT_FALSE(store.commit(id).has_value());
    const auto reaped =
        store.reap_abandoned(std::chrono::system_clock::now() + std::chrono::hours(24));
    ASSERT_TRUE(reaped);
    EXPECT_EQ(*reaped, 0U);
    store.discard(id);
    EXPECT_TRUE(std::filesystem::exists(ingest(id) / "durable"));
}

// An aborted session's write can still be on the pool when the next session on the same
// upload starts writing; run side by side, the two cut and extend the one data file under
// each other and the older can publish a durable offset past what the newer left there.
TEST_F(FsStoreTest, NextSessionWaitsForAnAbortedSessionsWrite) {
    auto pool = writer(2);
    net::OffloadPool& borrowed = *pool;
    FsStore store(deps(), std::move(pool), root, ulw::test::kLocalChunk);
    const auto data = ulw::test::pattern(2 * ulw::test::kLocalChunk, 3);
    const auto id = create(store, data.size());
    // Both threads held, so nothing the store hands over runs until the gates open.
    std::array<Gate, 2> gates;
    for (Gate& g : gates) {
        borrowed.submit(g);
        g.started.wait();
    }
    ulw::test::Observer first;
    auto aborted = store.open(id, 0, first);
    ASSERT_TRUE(aborted);
    ASSERT_EQ((*aborted)->write(std::span(data).first(ulw::test::kLocalChunk)),
              ulw::test::kLocalChunk);
    (*aborted)->abort();
    ulw::test::Observer second;
    auto next = store.open(id, 0, second);
    ASSERT_TRUE(next);
    const std::size_t half = ulw::test::kLocalChunk / 2;
    ASSERT_EQ((*next)->write(std::span(data).first(half)), half);
    // The two gates and the aborted write; the new session's write waits off the pool, where
    // a free thread cannot pick it up alongside the other.
    EXPECT_EQ(borrowed.in_flight(), 3U);
    for (Gate& g : gates) {
        g.release.count_down();
    }
    ASSERT_TRUE(upload(**next, second, std::span(data).subspan(half)));
    ASSERT_TRUE(store.commit(id));
    const auto stored = store.fetch_small(key, data.size());
    ASSERT_TRUE(stored);
    EXPECT_TRUE(*stored == data);
}

} // namespace
