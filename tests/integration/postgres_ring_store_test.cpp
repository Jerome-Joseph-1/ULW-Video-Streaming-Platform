#include "infra/postgres/ring_store.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"

#include "postgres_harness.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"
#include "support/reactor_harness.hpp"

#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// ADR-0129: the rooms' rings, on Postgres (migration 0026).
namespace {

using core::ports::RingStoreResult;
using core::ports::StoredRing;
using infra::postgres::Params;
using ulw::test::ScratchDatabase;

class PgRingStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        ScratchDatabase::open(db_);
        if (db_ == nullptr) {
            return;
        }
        auto r = net::make_reactor_with_fallback(net::ReactorKind::IoUring, clock_, 256);
        ASSERT_TRUE(r) << r.error();
        reactor_ = std::move(r->reactor);
        auto offload = net::OffloadPool::create(*reactor_, 1);
        ASSERT_TRUE(offload);
        offload_ = std::move(*offload);
        auto made = infra::postgres::PgRingStore::create(
            *reactor_, *offload_,
            {.conninfo = db_->conninfo(), .request_timeout = core::Millis{10'000}});
        ASSERT_TRUE(made) << made.error();
        store_ = std::move(*made);
    }
    void TearDown() override {
        offload_.reset();
        store_.reset();
        reactor_.reset();
    }

    template <class T, class Start> RingStoreResult<T> call(Start start) {
        std::optional<RingStoreResult<T>> got;
        start([&](RingStoreResult<T> r) noexcept { got = std::move(r); });
        EXPECT_TRUE(ulw::test::pump_until(
            *reactor_, [&] { return got.has_value(); }, std::chrono::seconds(10)));
        return got.value_or(std::unexpected(core::ports::RingStoreError::Unavailable));
    }

    StoredRing ring(std::string_view caller, std::vector<std::string_view> callees,
                    core::Millis lasts = core::Millis{45'000}) {
        StoredRing out{.room = core::RoomId::generate(clock_, random_),
                       .call = core::Uuid::v7(clock_, random_),
                       .caller = *core::UserId::parse(caller),
                       .group = callees.size() > 1,
                       .callees = {},
                       .started_at = std::chrono::floor<std::chrono::microseconds>(
                           std::chrono::system_clock::now()),
                       .expires_at = std::chrono::floor<std::chrono::microseconds>(
                           std::chrono::system_clock::now() + lasts)};
        for (const std::string_view c : callees) {
            out.callees.push_back(*core::UserId::parse(c));
        }
        return out;
    }

    ulw::test::FakeClock clock_;
    ulw::test::FakeRandom random_;
    std::unique_ptr<ScratchDatabase> db_;
    std::unique_ptr<net::IReactor> reactor_;
    std::unique_ptr<net::OffloadPool> offload_;
    std::unique_ptr<infra::postgres::PgRingStore> store_;
};

TEST_F(PgRingStoreTest, ARingIsKeptUntilItEndsAndReadByRoomAndByCallee) {
    if (db_ == nullptr) {
        return;
    }
    const StoredRing direct = ring("alice", {"bob"});
    const StoredRing group = ring("carol", {"bob", "dave"});
    ASSERT_TRUE(call<void>([&](auto d) { store_->put_ring(direct, std::move(d)); }));
    ASSERT_TRUE(call<void>([&](auto d) { store_->put_ring(group, std::move(d)); }));

    const auto read = call<std::optional<StoredRing>>(
        [&](auto d) { store_->ring_of(direct.room, std::move(d)); });
    ASSERT_TRUE(read && *read);
    EXPECT_EQ(**read, direct);
    const auto bobs = call<std::vector<StoredRing>>(
        [&](auto d) { store_->rings_for(direct.callees[0], std::move(d)); });
    ASSERT_TRUE(bobs);
    ASSERT_EQ(bobs->size(), 2U);
    const auto daves = call<std::vector<StoredRing>>(
        [&](auto d) { store_->rings_for(*core::UserId::parse("dave"), std::move(d)); });
    ASSERT_TRUE(daves);
    ASSERT_EQ(daves->size(), 1U);
    EXPECT_EQ(daves->front(), group);
    // The caller is not rung by their own call.
    const auto alices = call<std::vector<StoredRing>>(
        [&](auto d) { store_->rings_for(*core::UserId::parse("alice"), std::move(d)); });
    ASSERT_TRUE(alices);
    EXPECT_TRUE(alices->empty());

    // An end for another call of the room changes nothing; its own end removes it.
    ASSERT_TRUE(call<void>([&](auto d) {
        store_->end_ring(direct.room, core::Uuid::v7(clock_, random_), std::move(d));
    }));
    EXPECT_TRUE(call<std::optional<StoredRing>>([&](auto d) {
                    store_->ring_of(direct.room, std::move(d));
                }).value_or(std::nullopt));
    ASSERT_TRUE(
        call<void>([&](auto d) { store_->end_ring(direct.room, direct.call, std::move(d)); }));
    EXPECT_FALSE(call<std::optional<StoredRing>>([&](auto d) {
                     store_->ring_of(direct.room, std::move(d));
                 }).value_or(std::nullopt));
    // A new ring of the room takes the old one's place.
    StoredRing again = ring("carol", {"erin"});
    again.room = group.room;
    ASSERT_TRUE(call<void>([&](auto d) { store_->put_ring(again, std::move(d)); }));
    const auto replaced =
        call<std::optional<StoredRing>>([&](auto d) { store_->ring_of(group.room, std::move(d)); });
    ASSERT_TRUE(replaced && *replaced);
    EXPECT_EQ(**replaced, again);
}

TEST_F(PgRingStoreTest, ARingPastItsEndRingsNobodyAndIsForgottenAnHourOn) {
    if (db_ == nullptr) {
        return;
    }
    const StoredRing past = ring("alice", {"bob"}, core::Millis{-1'000});
    ASSERT_TRUE(call<void>([&](auto d) { store_->put_ring(past, std::move(d)); }));
    EXPECT_FALSE(call<std::optional<StoredRing>>([&](auto d) {
                     store_->ring_of(past.room, std::move(d));
                 }).value_or(std::nullopt));
    const auto bobs = call<std::vector<StoredRing>>(
        [&](auto d) { store_->rings_for(past.callees[0], std::move(d)); });
    ASSERT_TRUE(bobs);
    EXPECT_TRUE(bobs->empty());
    auto conn = db_->session();
    ASSERT_TRUE(conn.exec("UPDATE chat_call_rings SET expires_at = now() - interval '61 minutes'",
                          Params{}));
    const StoredRing other = ring("carol", {"dave"});
    ASSERT_TRUE(
        call<void>([&](auto d) { store_->end_ring(other.room, other.call, std::move(d)); }));
    const auto left = conn.exec("SELECT count(*) FROM chat_call_rings", Params{});
    ASSERT_TRUE(left);
    EXPECT_EQ(left->get(0, 0), "0");
}

} // namespace
