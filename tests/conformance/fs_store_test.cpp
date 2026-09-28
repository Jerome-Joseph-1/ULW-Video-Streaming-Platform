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

TEST_F(FsStoreTest, UnreadableMarkerIsAnErrorNotAnException) {
    FsStore store(deps(), root, ulw::test::kLocalChunk);
    const auto id = store.create(key, 100, *core::ContentType::parse("video/mp4"));
    ASSERT_TRUE(id);
    const auto ingest = root / "ingest" / id->backend_ref;
    // A symlink to itself: stat() fails with ELOOP even for root, which a permission bit
    // would not stop.
    std::filesystem::create_symlink("committed", ingest / "committed");
    EXPECT_FALSE(store.durable_offset(*id).has_value());
    EXPECT_FALSE(store.commit(*id).has_value());
    const auto reaped =
        store.reap_abandoned(std::chrono::system_clock::now() + std::chrono::hours(24));
    ASSERT_TRUE(reaped);
    EXPECT_EQ(*reaped, 0U);
    store.discard(*id);
    EXPECT_TRUE(std::filesystem::exists(ingest / "durable"));
}

} // namespace
