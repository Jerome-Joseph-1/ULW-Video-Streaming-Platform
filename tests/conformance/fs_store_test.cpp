#include "infra/storage/fs_store.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"

#include "storage_harness.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"

#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
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
        auto p = net::OffloadPool::create(*reactor, 1);
        ASSERT_TRUE(p);
        pool = std::move(*p);
    }
    void TearDown() override {
        pool.reset();
        reactor.reset();
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    [[nodiscard]] FsStore::Deps deps() {
        return {.reactor = *reactor, .pool = *pool, .clock = clock, .random = random};
    }

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
    std::unique_ptr<net::OffloadPool> pool;
};

TEST_F(FsStoreTest, ZeroChunkSizeIsAProgrammingError) {
    EXPECT_THROW(FsStore(deps(), root, 0), std::invalid_argument);
}

TEST_F(FsStoreTest, CommitRefusesADataFileShorterThanTheObject) {
    FsStore store(deps(), root, ulw::test::kLocalChunk);
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
    FsStore store(deps(), root, ulw::test::kLocalChunk);
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

} // namespace
