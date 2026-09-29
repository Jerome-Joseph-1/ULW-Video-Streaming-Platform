#include "infra/postgres/room_store.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "postgres_harness.hpp"
#include "support/reactor_harness.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using infra::postgres::Params;
using infra::postgres::PgRoomStore;
using rt::OwnedRoom;
using rt::Ownership;
using rt::StoreResult;
using ulw::test::scalar;
using ulw::test::ScratchDatabase;
using Seq = StoreResult<std::optional<std::uint64_t>>;
using Address = StoreResult<std::optional<std::string>>;

core::NodeId node(std::string_view name) {
    return *core::NodeId::parse(name);
}

class Listener final : public rt::IOwnershipListener {
public:
    void on_owner_changed(const core::RoomId& room, const Ownership& owner) noexcept override {
        changes.emplace_back(room, owner);
    }
    void on_resync() noexcept override { ++resyncs; }

    std::vector<std::pair<core::RoomId, Ownership>> changes;
    int resyncs = 0;
};

class RoomStoreTest : public ::testing::TestWithParam<net::ReactorKind> {
protected:
    void SetUp() override {
        ScratchDatabase::open(db_);
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        auto reactor = net::make_reactor(GetParam(), clock_, 1024);
        ASSERT_TRUE(reactor);
        reactor_ = std::move(*reactor);
        auto offload = net::OffloadPool::create(*reactor_, 1);
        ASSERT_TRUE(offload);
        offload_ = std::move(*offload);
        auto store = PgRoomStore::create(*reactor_, *offload_, {.conninfo = db_->conninfo()});
        ASSERT_TRUE(store) << store.error();
        store_ = std::move(*store);
        conn_.emplace(db_->session());
    }

    void TearDown() override {
        offload_.reset();
        store_.reset();
        reactor_.reset();
    }

    // Pumps the loop until the store answers.
    template <class T, class Call> StoreResult<T> ask(Call call) {
        std::optional<StoreResult<T>> answer;
        call([&answer](StoreResult<T> r) noexcept { answer = std::move(r); });
        if (!ulw::test::pump_until(*reactor_, [&] { return answer.has_value(); })) {
            ADD_FAILURE() << "the store never answered";
            return std::unexpected(rt::StoreError::Unavailable);
        }
        return std::move(*answer);
    }

    StoreResult<Ownership> resolve(const core::RoomId& room, const core::NodeId& by) {
        return ask<Ownership>([&](auto done) { store_->resolve(room, by, std::move(done)); });
    }

    StoreResult<std::vector<OwnedRoom>> claim_stale(std::vector<core::RoomId> rooms,
                                                    const core::NodeId& by) {
        return ask<std::vector<OwnedRoom>>(
            [&](auto done) { store_->claim_stale(std::move(rooms), by, std::move(done)); });
    }

    StoreResult<std::optional<std::uint64_t>> append(const core::RoomId& room,
                                                     std::uint64_t generation) {
        // A key of its own each time: a repeated key is the same message, and takes no seq.
        const std::string key = "k" + std::to_string(++keys_);
        return ask<std::optional<std::uint64_t>>([&](auto done) {
            store_->append(room, generation,
                           {.sender = *core::UserId::parse("alice"),
                            .key = *rt::MessageKey::parse(key),
                            .body = {}},
                           std::move(done));
        });
    }

    StoreResult<std::vector<core::RoomId>> heartbeat(const core::NodeId& by,
                                                     std::vector<OwnedRoom> rooms) {
        return ask<std::vector<core::RoomId>>(
            [&](auto done) { store_->heartbeat(by, run_, std::move(rooms), std::move(done)); });
    }

    // What an owner that stopped beating looks like, without waiting for it.
    void go_quiet(const core::RoomId& room) {
        ASSERT_TRUE(
            conn_->exec("UPDATE room_assignments SET heartbeat_at = now() - interval '6 seconds' "
                        "WHERE room_id = $1",
                        Params{}.add_uuid(room.uuid())));
    }

    // The generation room_state fences appends on.
    std::string fence(const core::RoomId& room) {
        return scalar(*conn_, "SELECT owner_generation FROM room_state WHERE room_id = $1",
                      Params{}.add_uuid(room.uuid()));
    }

    std::string last_seq(const core::RoomId& room) {
        return scalar(*conn_, "SELECT last_seq FROM room_state WHERE room_id = $1",
                      Params{}.add_uuid(room.uuid()));
    }

    core::RoomId new_room() { return core::RoomId::generate(clock_, random_); }

    os::SystemClock clock_;
    os::SystemRandom random_;
    std::unique_ptr<ScratchDatabase> db_;
    std::unique_ptr<net::IReactor> reactor_;
    std::unique_ptr<net::OffloadPool> offload_;
    std::unique_ptr<PgRoomStore> store_;
    std::optional<infra::postgres::SyncConnection> conn_;
    // This run of every node the tests play.
    const core::Uuid run_ = core::Uuid::v7(clock_, random_);
    std::uint64_t keys_ = 0;
    const core::NodeId a_ = node("chat-a");
    const core::NodeId b_ = node("chat-b");
    const core::NodeId c_ = node("chat-c");
};

TEST_P(RoomStoreTest, TheFirstNodeToResolveARoomCreatesItUnderGenerationOne) {
    const core::RoomId room = new_room();
    EXPECT_EQ(resolve(room, a_), (Ownership{.node = a_, .generation = 1}));
    EXPECT_EQ(scalar(*conn_,
                     "SELECT concat_ws(' ', owner_generation, last_seq, kind, delivery) "
                     "FROM room_state WHERE room_id = $1",
                     Params{}.add_uuid(room.uuid())),
              "1 0 group_chat durable");
}

TEST_P(RoomStoreTest, ARoomTakesTheKindRecordedForIt) {
    // A stream's room: only its kind of id (version 8) can be recorded live (ADR-0057).
    std::string text = new_room().to_string();
    text[14] = '8';
    const core::RoomId room = *core::RoomId::parse(text);
    ASSERT_TRUE(
        conn_->exec("INSERT INTO chat_rooms (room_id, kind) VALUES ($1, 'stream_live_chat')",
                    Params{}.add_uuid(room.uuid())));
    ASSERT_TRUE(resolve(room, a_));
    EXPECT_EQ(scalar(*conn_,
                     "SELECT concat_ws(' ', kind, delivery) FROM room_state WHERE room_id = $1",
                     Params{}.add_uuid(room.uuid())),
              "stream_live_chat lossy");
}

TEST_P(RoomStoreTest, AnOwnerWhoseHeartbeatIsFreshKeepsItsRoom) {
    const core::RoomId room = new_room();
    ASSERT_TRUE(resolve(room, a_));
    EXPECT_EQ(resolve(room, b_), (Ownership{.node = a_, .generation = 1}));
    EXPECT_EQ(claim_stale({room}, b_), std::vector<OwnedRoom>{});
    EXPECT_EQ(fence(room), "1");
}

TEST_P(RoomStoreTest, AQuietOwnersRoomGoesToTheNextNodeUnderAHigherGeneration) {
    const core::RoomId room = new_room();
    ASSERT_TRUE(resolve(room, a_));
    go_quiet(room);
    EXPECT_EQ(resolve(room, b_), (Ownership{.node = b_, .generation = 2}));
    // The fence moved with the owner.
    EXPECT_EQ(fence(room), "2");
}

TEST_P(RoomStoreTest, OfTwoNodesClaimingAQuietRoomExactlyOneGetsIt) {
    const core::RoomId room = new_room();
    ASSERT_TRUE(resolve(room, a_));
    go_quiet(room);
    std::optional<StoreResult<std::vector<OwnedRoom>>> by_b;
    std::optional<StoreResult<std::vector<OwnedRoom>>> by_c;
    store_->claim_stale({room}, b_, [&](auto r) noexcept { by_b = std::move(r); });
    store_->claim_stale({room}, c_, [&](auto r) noexcept { by_c = std::move(r); });
    ASSERT_TRUE(ulw::test::pump_until(*reactor_, [&] { return by_b && by_c; }));
    ASSERT_TRUE(*by_b && *by_c);
    EXPECT_EQ((*by_b)->size() + (*by_c)->size(), 1U);
    const auto& winner = (*by_b)->empty() ? **by_c : **by_b;
    EXPECT_EQ(winner.front().generation, 2U);
    EXPECT_EQ(fence(room), "2");
}

TEST_P(RoomStoreTest, AFormerOwnersAppendUpdatesNoRowsAndTheNewOwnersCarriesOn) {
    const core::RoomId room = new_room();
    ASSERT_TRUE(resolve(room, a_));
    EXPECT_EQ(append(room, 1), Seq{1});
    go_quiet(room);
    ASSERT_EQ(resolve(room, b_), (Ownership{.node = b_, .generation = 2}));

    EXPECT_EQ(append(room, 1), Seq{std::nullopt});
    EXPECT_EQ(last_seq(room), "1");
    EXPECT_EQ(append(room, 2), Seq{2});
    EXPECT_EQ(last_seq(room), "2");
}

TEST_P(RoomStoreTest, AHeartbeatRenewsOnlyRoomsStillHeldUnderTheirGeneration) {
    const core::RoomId kept = new_room();
    const core::RoomId taken = new_room();
    ASSERT_TRUE(resolve(kept, a_));
    ASSERT_TRUE(resolve(taken, a_));
    go_quiet(taken);
    ASSERT_TRUE(resolve(taken, b_));

    EXPECT_EQ(heartbeat(a_, {{.room = kept, .generation = 1}, {.room = taken, .generation = 1}}),
              std::vector<core::RoomId>{kept});
    EXPECT_EQ(heartbeat(b_, {{.room = taken, .generation = 2}}), std::vector<core::RoomId>{taken});
    EXPECT_EQ(heartbeat(a_, {}), std::vector<core::RoomId>{});
}

TEST_P(RoomStoreTest, AHeartbeatKeepsAQuietRoomFromBeingClaimed) {
    const core::RoomId room = new_room();
    ASSERT_TRUE(resolve(room, a_));
    go_quiet(room);
    ASSERT_EQ(heartbeat(a_, {{.room = room, .generation = 1}}), std::vector<core::RoomId>{room});
    EXPECT_EQ(claim_stale({room}, b_), std::vector<OwnedRoom>{});
}

TEST_P(RoomStoreTest, AReleasedRoomIsClaimableAtOnceButOnlyUnderItsGeneration) {
    const core::RoomId released = new_room();
    const core::RoomId other = new_room();
    ASSERT_TRUE(resolve(released, a_));
    ASSERT_TRUE(resolve(other, a_));
    ASSERT_TRUE(ask<void>([&](auto done) {
        store_->release(a_, {{.room = released, .generation = 1}, {.room = other, .generation = 9}},
                        std::move(done));
    }));
    const auto claimed = claim_stale({released, other}, b_);
    ASSERT_TRUE(claimed);
    ASSERT_EQ(claimed->size(), 1U);
    EXPECT_EQ(claimed->front().room, released);
    EXPECT_EQ(claimed->front().generation, 2U);
}

TEST_P(RoomStoreTest, ANodeFindingARoomRecordedAsItsOwnTakesItAgainUnderANewGeneration) {
    const core::RoomId room = new_room();
    ASSERT_TRUE(resolve(room, a_));
    // A restarted node announces itself anew, and does not know what its earlier run wrote
    // under generation 1.
    ASSERT_TRUE(ask<void>(
        [&](auto done) { store_->advertise(a_, "127.0.0.1:9201", run_, std::move(done)); }));
    EXPECT_EQ(resolve(room, a_), (Ownership{.node = a_, .generation = 2}));
    EXPECT_EQ(append(room, 1), Seq{std::nullopt});
}

// Two sessions of one node, as its sweep and a lookup run side by side: the sweep's claim
// holds the row when the lookup arrives, and the lookup must find the node's own fresh claim,
// not take the room again and fence it out.
TEST_P(RoomStoreTest, ALookupByANodeWhoseOwnClaimIsInFlightKeepsThatClaimsGeneration) {
    const core::RoomId room = new_room();
    ASSERT_TRUE(resolve(room, a_));
    go_quiet(room);
    ASSERT_TRUE(ask<void>(
        [&](auto done) { store_->advertise(b_, "127.0.0.1:9201", run_, std::move(done)); }));

    auto sweep = db_->session();
    ASSERT_TRUE(sweep.exec("BEGIN"));
    ASSERT_EQ(scalar(sweep,
                     "WITH claimed AS ("
                     "  UPDATE room_assignments SET owner_node = $1,"
                     "         owner_generation = owner_generation + 1, heartbeat_at = now()"
                     "   WHERE room_id = $2 AND heartbeat_at < now() - interval '5 seconds'"
                     "  RETURNING owner_generation)"
                     "UPDATE room_state SET owner_generation = claimed.owner_generation"
                     "  FROM claimed WHERE room_state.room_id = $2 "
                     "RETURNING room_state.owner_generation",
                     Params{}.add_text(b_.view()).add_uuid(room.uuid())),
              "2");

    std::optional<StoreResult<Ownership>> looked_up;
    store_->resolve(room, b_, [&](StoreResult<Ownership> r) noexcept { looked_up = r; });
    // The lookup reaches the row and waits on the sweep's lock.
    ASSERT_TRUE(ulw::test::pump_until(*reactor_, [&] {
        return scalar(*conn_, "SELECT count(*) FROM pg_stat_activity WHERE "
                              "datname = current_database() AND wait_event_type = 'Lock'") == "1";
    }));
    ASSERT_TRUE(sweep.exec("COMMIT"));
    ASSERT_TRUE(ulw::test::pump_until(*reactor_, [&] { return looked_up.has_value(); }));

    EXPECT_EQ(*looked_up, (Ownership{.node = b_, .generation = 2}));
    EXPECT_EQ(fence(room), "2");
    EXPECT_EQ(append(room, 2), Seq{1});
}

TEST_P(RoomStoreTest, ReadingOwnersCreatesAndClaimsNothing) {
    using Owners = std::vector<std::pair<core::RoomId, Ownership>>;
    const core::RoomId kept = new_room();
    const core::RoomId taken = new_room();
    const core::RoomId quiet = new_room();
    const core::RoomId unknown = new_room();
    ASSERT_TRUE(resolve(kept, a_));
    ASSERT_TRUE(resolve(taken, a_));
    go_quiet(taken);
    ASSERT_TRUE(resolve(taken, b_));
    ASSERT_TRUE(resolve(quiet, a_));
    go_quiet(quiet);

    auto owners = ask<Owners>(
        [&](auto done) { store_->read_owners({kept, taken, quiet, unknown}, std::move(done)); });
    ASSERT_TRUE(owners);
    std::ranges::sort(*owners, {}, [](const auto& o) { return o.first.to_string(); });
    Owners expected{{kept, {.node = a_, .generation = 1}},
                    {taken, {.node = b_, .generation = 2}},
                    {quiet, {.node = a_, .generation = 1}}};
    std::ranges::sort(expected, {}, [](const auto& o) { return o.first.to_string(); });
    EXPECT_EQ(*owners, expected);
    // A quiet room is left for a claim, and an unknown one is not created.
    EXPECT_EQ(fence(quiet), "1");
    EXPECT_EQ(scalar(*conn_, "SELECT count(*) FROM room_assignments WHERE room_id = $1",
                     Params{}.add_uuid(unknown.uuid())),
              "0");
}

TEST_P(RoomStoreTest, CreationsAndTakeoversAreAnnouncedButHeartbeatsAreNot) {
    Listener listener;
    store_->watch(listener);
    ASSERT_TRUE(ulw::test::pump_until(*reactor_, [&] { return listener.resyncs == 1; }));

    const core::RoomId room = new_room();
    ASSERT_TRUE(resolve(room, a_));
    ASSERT_TRUE(heartbeat(a_, {{.room = room, .generation = 1}}));
    go_quiet(room);
    ASSERT_TRUE(resolve(room, b_));
    ASSERT_TRUE(ulw::test::pump_until(*reactor_, [&] { return listener.changes.size() >= 2; }));
    // Anything the heartbeat had announced would have come in between, on the same session.
    ulw::test::pump_pending(*reactor_);
    ASSERT_EQ(listener.changes.size(), 2U);
    EXPECT_EQ(listener.changes[0].first, room);
    EXPECT_EQ(listener.changes[0].second, (Ownership{.node = a_, .generation = 1}));
    EXPECT_EQ(listener.changes[1].second, (Ownership{.node = b_, .generation = 2}));
}

TEST_P(RoomStoreTest, ALostListeningSessionIsReportedAsAResync) {
    Listener listener;
    store_->watch(listener);
    ASSERT_TRUE(ulw::test::pump_until(*reactor_, [&] { return listener.resyncs == 1; }));
    ASSERT_EQ(scalar(*conn_, "SELECT count(pg_terminate_backend(pid)) FROM pg_stat_activity "
                             "WHERE datname = current_database() "
                             "AND application_name = 'ulw-rooms-listen'"),
              "1");
    EXPECT_TRUE(ulw::test::pump_until(*reactor_, [&] { return listener.resyncs == 2; }));
}

TEST_P(RoomStoreTest, AForeignPayloadOnTheChannelIsTreatedAsAResync) {
    Listener listener;
    store_->watch(listener);
    ASSERT_TRUE(ulw::test::pump_until(*reactor_, [&] { return listener.resyncs == 1; }));
    ASSERT_TRUE(conn_->exec("SELECT pg_notify('room_owner', 'not a notice')"));
    EXPECT_TRUE(ulw::test::pump_until(*reactor_, [&] { return listener.resyncs == 2; }));
    EXPECT_TRUE(listener.changes.empty());
}

TEST_P(RoomStoreTest, NodesFindEachOthersLatestAddress) {
    const auto advertise = [&](const core::NodeId& n, std::string address) {
        return ask<void>(
            [&](auto done) { store_->advertise(n, std::move(address), run_, std::move(done)); });
    };
    const auto find = [&](const core::NodeId& n) {
        return ask<std::optional<std::string>>(
            [&](auto done) { store_->find_address(n, std::move(done)); });
    };
    ASSERT_TRUE(advertise(a_, "10.0.0.1:9201"));
    EXPECT_EQ(find(a_), Address{"10.0.0.1:9201"});
    ASSERT_TRUE(advertise(a_, "10.0.0.7:9201"));
    EXPECT_EQ(find(a_), Address{"10.0.0.7:9201"});
    EXPECT_EQ(find(b_), Address{std::nullopt});
}

TEST_P(RoomStoreTest, ANameIsHeldByOneLiveRunAtATime) {
    const core::Uuid other = core::Uuid::v7(clock_, random_);
    const auto advertise = [&](const core::Uuid& run) {
        return ask<void>(
            [&](auto done) { store_->advertise(a_, "10.0.0.1:9201", run, std::move(done)); });
    };
    ASSERT_TRUE(advertise(run_));
    const std::string started = scalar(*conn_, "SELECT started_at FROM chat_nodes");
    // The same run again: still its name, from its own start.
    ASSERT_TRUE(advertise(run_));
    EXPECT_EQ(scalar(*conn_, "SELECT started_at FROM chat_nodes"), started);
    // Another process under the same name, while this run beats: refused.
    EXPECT_EQ(advertise(other), std::unexpected(rt::StoreError::NodeTaken));

    // Quiet past the stale bound, then a heartbeat: held again.
    ASSERT_TRUE(conn_->exec("UPDATE chat_nodes SET seen_at = now() - interval '6 seconds'"));
    ASSERT_TRUE(heartbeat(a_, {}));
    EXPECT_EQ(advertise(other), std::unexpected(rt::StoreError::NodeTaken));

    // Quiet past the stale bound for good: the other run takes the name, from a new start.
    ASSERT_TRUE(conn_->exec("UPDATE chat_nodes SET seen_at = now() - interval '6 seconds'"));
    EXPECT_TRUE(advertise(other));
    EXPECT_EQ(scalar(*conn_, "SELECT incarnation FROM chat_nodes"), other.to_string());
    EXPECT_NE(scalar(*conn_, "SELECT started_at FROM chat_nodes"), started);
}

INSTANTIATE_TEST_SUITE_P(Reactors, RoomStoreTest,
                         ::testing::Values(net::ReactorKind::IoUring, net::ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
