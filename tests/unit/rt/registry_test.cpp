#include "rt/registry.hpp"

#include "fake_room_store.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using MediaAnswer = rt::StoreResult<std::optional<std::uint64_t>>;

using rt::AppendError;
using rt::OwnedRoom;
using rt::Ownership;
using rt::OwnerWrite;
using ulw::test::FakeRoomStore;

core::NodeId node(std::string_view name) {
    return *core::NodeId::parse(name);
}

struct Fence {
    core::RoomId room;
    std::uint64_t generation;
    OwnerWrite write;
};

class Observer final : public rt::IRegistryObserver {
public:
    void on_owner_changed(const core::RoomId& room, const Ownership& owner) noexcept override {
        changes.emplace_back(room, owner);
    }
    void on_fenced_out(const core::RoomId& room, std::uint64_t generation,
                       OwnerWrite write) noexcept override {
        fences.push_back({.room = room, .generation = generation, .write = write});
    }

    std::vector<std::pair<core::RoomId, Ownership>> changes;
    std::vector<Fence> fences;
};

class RegistryTest : public ::testing::Test {
protected:
    core::RoomId new_room() { return core::RoomId::generate(clock_, random_); }

    // Asks for the owner and answers as the store would.
    void resolve_to(const core::RoomId& room, const Ownership& owner) {
        registry_.resolve(room, [](const rt::StoreResult<Ownership>&) noexcept {});
        ASSERT_EQ(store_.resolves.size(), 1U);
        FakeRoomStore::take(store_.resolves).done(owner);
    }

    void own(const core::RoomId& room, std::uint64_t generation) {
        resolve_to(room, {.node = self_, .generation = generation});
    }

    ulw::test::FakeClock clock_;
    ulw::test::FakeRandom random_;
    FakeRoomStore store_;
    const rt::Outgoing message_{
        .sender = *core::UserId::parse("alice"), .key = *rt::MessageKey::parse("k1"), .body = {}};
    Observer observer_;
    const core::NodeId self_ = node("chat-a");
    const core::NodeId other_ = node("chat-b");
    rt::RoomRegistry registry_{store_, clock_, self_, core::Uuid::v7(clock_, random_), observer_};
};

TEST_F(RegistryTest, ConcurrentLookupsOfOneRoomShareOneStoreCall) {
    const core::RoomId room = new_room();
    std::vector<rt::StoreResult<Ownership>> answers;
    for (int i = 0; i < 3; ++i) {
        registry_.resolve(
            room, [&](rt::StoreResult<Ownership> r) noexcept { answers.push_back(std::move(r)); });
    }
    ASSERT_EQ(store_.resolves.size(), 1U);
    EXPECT_TRUE(answers.empty());

    const Ownership b{.node = other_, .generation = 4};
    FakeRoomStore::take(store_.resolves).done(b);
    ASSERT_EQ(answers.size(), 3U);
    for (const auto& a : answers) {
        EXPECT_EQ(a, b);
    }
    EXPECT_EQ(registry_.known_owner(room), b);
    EXPECT_FALSE(registry_.owned(room));
    ASSERT_EQ(observer_.changes.size(), 1U);
    EXPECT_EQ(observer_.changes[0].second, b);
}

TEST_F(RegistryTest, AFailedLookupLeavesNothingCachedAndTheNextOneAsksAgain) {
    const core::RoomId room = new_room();
    std::optional<rt::StoreResult<Ownership>> answer;
    registry_.resolve(room, [&](rt::StoreResult<Ownership> r) noexcept { answer = r; });
    FakeRoomStore::take(store_.resolves).done(std::unexpected(rt::StoreError::Unavailable));
    ASSERT_TRUE(answer);
    EXPECT_EQ(*answer, std::unexpected(rt::StoreError::Unavailable));
    EXPECT_FALSE(registry_.known_owner(room));
    registry_.resolve(room, [](const rt::StoreResult<Ownership>&) noexcept {});
    EXPECT_EQ(store_.resolves.size(), 1U);
}

TEST_F(RegistryTest, CreatingARoomOwnsItWithoutCountingAReassignment) {
    const core::RoomId room = new_room();
    own(room, 1);
    EXPECT_EQ(registry_.owned(room), 1U);
    EXPECT_EQ(registry_.known_owner(room), (Ownership{.node = self_, .generation = 1}));
    EXPECT_EQ(registry_.counters().reassignments, 0U);
    ASSERT_EQ(observer_.changes.size(), 1U);
    EXPECT_EQ(observer_.changes[0].second.node, self_);
}

TEST_F(RegistryTest, TakingARoomOverCountsAReassignment) {
    const core::RoomId room = new_room();
    own(room, 3);
    EXPECT_EQ(registry_.owned(room), 3U);
    EXPECT_EQ(registry_.counters().reassignments, 1U);
}

TEST_F(RegistryTest, AnAppendIsFencedOnTheGenerationThisNodeOwnsTheRoomUnder) {
    const core::RoomId room = new_room();
    own(room, 2);
    std::optional<std::expected<std::uint64_t, AppendError>> result;
    ASSERT_TRUE(registry_.append(room, message_, [&](auto r) noexcept { result = r; }));
    ASSERT_EQ(store_.appends.size(), 1U);
    EXPECT_EQ(store_.appends.front().room, room);
    EXPECT_EQ(store_.appends.front().generation, 2U);
    FakeRoomStore::take(store_.appends).done(std::optional<std::uint64_t>{17});
    EXPECT_EQ(result, 17U);
    EXPECT_EQ(registry_.owned(room), 2U);
    EXPECT_TRUE(observer_.fences.empty());
}

TEST_F(RegistryTest, AFencedAppendEndsOwnershipAtOnceAndIsReported) {
    const core::RoomId room = new_room();
    own(room, 2);
    std::optional<std::expected<std::uint64_t, AppendError>> result;
    ASSERT_TRUE(registry_.append(room, message_, [&](auto r) noexcept { result = r; }));
    FakeRoomStore::take(store_.appends).done(std::optional<std::uint64_t>{});

    EXPECT_EQ(result, std::unexpected(AppendError::Fenced));
    EXPECT_FALSE(registry_.owned(room));
    EXPECT_FALSE(registry_.known_owner(room));
    EXPECT_EQ(registry_.counters().fenced_writes, 1U);
    ASSERT_EQ(observer_.fences.size(), 1U);
    EXPECT_EQ(observer_.fences[0].room, room);
    EXPECT_EQ(observer_.fences[0].generation, 2U);
    EXPECT_EQ(observer_.fences[0].write, OwnerWrite::Append);
    // No second write under the old generation, and none retried.
    EXPECT_FALSE(registry_.append(room, message_, [](auto) noexcept {}));
    EXPECT_TRUE(store_.appends.empty());
}

TEST_F(RegistryTest, TheMediaGenerationIsReadAndMovedOnUnderTheOwnersGeneration) {
    const core::RoomId room = new_room();
    EXPECT_FALSE(registry_.media_generation(room, rt::MediaStep::Read, [](auto) noexcept {}));
    EXPECT_TRUE(store_.media.empty());
    own(room, 4);
    std::optional<rt::StoreResult<std::optional<std::uint64_t>>> result;
    ASSERT_TRUE(
        registry_.media_generation(room, rt::MediaStep::Advance, [&](auto r) noexcept { result = r; }));
    ASSERT_EQ(store_.media.size(), 1U);
    EXPECT_EQ(store_.media.front().generation, 4U);
    EXPECT_EQ(store_.media.front().step, rt::MediaStep::Advance);
    FakeRoomStore::take(store_.media).done(MediaAnswer{std::optional<std::uint64_t>{7}});
    EXPECT_EQ(result, MediaAnswer{std::optional<std::uint64_t>{7}});
    EXPECT_EQ(registry_.owned(room), 4U);

    // The store not answering says nothing of the room's owner.
    ASSERT_TRUE(
        registry_.media_generation(room, rt::MediaStep::Read, [&](auto r) noexcept { result = r; }));
    FakeRoomStore::take(store_.media).done(std::unexpected(rt::StoreError::Unavailable));
    EXPECT_EQ(result, std::unexpected(rt::StoreError::Unavailable));
    EXPECT_EQ(registry_.owned(room), 4U);
    EXPECT_TRUE(observer_.fences.empty());
}

TEST_F(RegistryTest, AFencedMediaGenerationEndsOwnershipAsAnyOwnerWriteDoes) {
    const core::RoomId room = new_room();
    own(room, 2);
    std::optional<rt::StoreResult<std::optional<std::uint64_t>>> result;
    ASSERT_TRUE(
        registry_.media_generation(room, rt::MediaStep::Advance, [&](auto r) noexcept { result = r; }));
    FakeRoomStore::take(store_.media).done(MediaAnswer{std::optional<std::uint64_t>{}});
    EXPECT_EQ(result, MediaAnswer{std::optional<std::uint64_t>{}});
    EXPECT_FALSE(registry_.owned(room));
    ASSERT_EQ(observer_.fences.size(), 1U);
    EXPECT_EQ(observer_.fences[0].write, OwnerWrite::MediaGeneration);
}

TEST_F(RegistryTest, EveryFencedWriteInFlightIsReportedNotJustTheFirst) {
    const core::RoomId room = new_room();
    own(room, 2);
    int fenced = 0;
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(registry_.append(room, message_, [&](auto r) noexcept {
            fenced += r == std::unexpected(AppendError::Fenced) ? 1 : 0;
        }));
    }
    FakeRoomStore::take(store_.appends).done(std::optional<std::uint64_t>{});
    FakeRoomStore::take(store_.appends).done(std::optional<std::uint64_t>{});
    EXPECT_EQ(fenced, 2);
    EXPECT_EQ(registry_.counters().fenced_writes, 2U);
    EXPECT_EQ(observer_.fences.size(), 2U);
}

TEST_F(RegistryTest, AnUnansweredAppendKeepsOwnership) {
    const core::RoomId room = new_room();
    own(room, 2);
    std::optional<std::expected<std::uint64_t, AppendError>> result;
    ASSERT_TRUE(registry_.append(room, message_, [&](auto r) noexcept { result = r; }));
    FakeRoomStore::take(store_.appends).done(std::unexpected(rt::StoreError::Unavailable));
    EXPECT_EQ(result, std::unexpected(AppendError::Unavailable));
    EXPECT_EQ(registry_.owned(room), 2U);
    EXPECT_EQ(registry_.counters().fenced_writes, 0U);
}

TEST_F(RegistryTest, ATakeoverNoticeUpdatesRoutingButOwnershipEndsOnlyAtTheFence) {
    const core::RoomId room = new_room();
    own(room, 1);
    const Ownership b{.node = other_, .generation = 2};
    store_.listener().on_owner_changed(room, b);

    // Still the owner as far as writes go: the next one finds out.
    EXPECT_EQ(registry_.owned(room), 1U);
    ASSERT_TRUE(registry_.append(room, message_, [](auto) noexcept {}));
    EXPECT_EQ(store_.appends.front().generation, 1U);
    FakeRoomStore::take(store_.appends).done(std::optional<std::uint64_t>{});

    EXPECT_FALSE(registry_.owned(room));
    EXPECT_EQ(registry_.known_owner(room), b);
}

TEST_F(RegistryTest, AnOlderGenerationNeverReplacesANewerOne) {
    const core::RoomId room = new_room();
    const Ownership newer{.node = other_, .generation = 5};
    resolve_to(room, newer);
    store_.listener().on_owner_changed(room, {.node = node("chat-c"), .generation = 4});
    EXPECT_EQ(registry_.known_owner(room), newer);
    EXPECT_EQ(observer_.changes.size(), 1U);
}

TEST_F(RegistryTest, NoticesAboutRoomsNobodyHereUsesAreNotKept) {
    const core::RoomId room = new_room();
    store_.listener().on_owner_changed(room, {.node = other_, .generation = 1});
    EXPECT_FALSE(registry_.known_owner(room));
    EXPECT_TRUE(observer_.changes.empty());

    registry_.set_interest(room, true);
    store_.listener().on_owner_changed(room, {.node = other_, .generation = 1});
    EXPECT_EQ(registry_.known_owner(room), (Ownership{.node = other_, .generation = 1}));
    EXPECT_EQ(observer_.changes.size(), 1U);
}

TEST_F(RegistryTest, ANoticeOfThisNodesOwnClaimIsNoAnswerUntilTheClaimReturns) {
    const core::RoomId room = new_room();
    registry_.set_interest(room, true);
    store_.listener().on_owner_changed(room, {.node = self_, .generation = 3});
    // Without the claim's answer this node does not own the room, and must not route to itself.
    EXPECT_FALSE(registry_.known_owner(room));
    EXPECT_FALSE(registry_.owned(room));
    EXPECT_TRUE(observer_.changes.empty());
}

TEST_F(RegistryTest, AHeartbeatThatSkipsARoomFencesThatRoomOut) {
    const core::RoomId kept = new_room();
    const core::RoomId lost = new_room();
    own(kept, 1);
    own(lost, 4);
    registry_.tick();
    ASSERT_EQ(store_.heartbeats.size(), 1U);
    EXPECT_EQ(store_.heartbeats.front().rooms.size(), 2U);
    FakeRoomStore::take(store_.heartbeats).done(std::vector<core::RoomId>{kept});

    EXPECT_EQ(registry_.owned(kept), 1U);
    EXPECT_FALSE(registry_.owned(lost));
    ASSERT_EQ(observer_.fences.size(), 1U);
    EXPECT_EQ(observer_.fences[0].room, lost);
    EXPECT_EQ(observer_.fences[0].generation, 4U);
    EXPECT_EQ(observer_.fences[0].write, OwnerWrite::Heartbeat);
}

TEST_F(RegistryTest, ARoomTakenWhileAHeartbeatRunsIsNotJudgedByIt) {
    registry_.tick();
    const core::RoomId room = new_room();
    own(room, 1);
    FakeRoomStore::take(store_.heartbeats).done(std::vector<core::RoomId>{});
    EXPECT_EQ(registry_.owned(room), 1U);
    EXPECT_TRUE(observer_.fences.empty());
}

TEST_F(RegistryTest, HealthFollowsTheLastHeartbeatThatReachedTheStore) {
    EXPECT_FALSE(registry_.healthy());
    registry_.tick();
    FakeRoomStore::take(store_.heartbeats).done(std::vector<core::RoomId>{});
    EXPECT_TRUE(registry_.healthy());

    clock_.advance(rt::kOwnerStaleAfter - core::Millis{1});
    EXPECT_TRUE(registry_.healthy());
    registry_.tick();
    FakeRoomStore::take(store_.heartbeats).done(std::unexpected(rt::StoreError::Unavailable));
    clock_.advance(core::Millis{1});
    EXPECT_FALSE(registry_.healthy());
}

TEST_F(RegistryTest, ATickWhileTheLastOneRunsSendsNothingMore) {
    const core::RoomId room = new_room();
    registry_.set_interest(room, true);
    registry_.tick();
    registry_.tick();
    EXPECT_EQ(store_.heartbeats.size(), 1U);
    EXPECT_EQ(store_.claims.size(), 1U);
    FakeRoomStore::take(store_.heartbeats).done(std::vector<core::RoomId>{});
    FakeRoomStore::take(store_.claims).done(std::vector<OwnedRoom>{});
    registry_.tick();
    EXPECT_EQ(store_.heartbeats.size(), 1U);
    EXPECT_EQ(store_.claims.size(), 1U);
}

TEST_F(RegistryTest, TheSweepClaimsRoomsOfInterestThisNodeDoesNotOwn) {
    const core::RoomId mine = new_room();
    const core::RoomId quiet = new_room();
    const core::RoomId busy = new_room();
    own(mine, 1);
    for (const auto& room : {mine, quiet, busy}) {
        registry_.set_interest(room, true);
    }
    registry_.tick();
    ASSERT_EQ(store_.claims.size(), 1U);
    auto candidates = store_.claims.front().rooms;
    std::ranges::sort(candidates);
    auto expected = std::vector<core::RoomId>{quiet, busy};
    std::ranges::sort(expected);
    EXPECT_EQ(candidates, expected);

    FakeRoomStore::take(store_.claims)
        .done(std::vector<OwnedRoom>{{.room = quiet, .generation = 6}});
    EXPECT_EQ(registry_.owned(quiet), 6U);
    EXPECT_FALSE(registry_.owned(busy));
    EXPECT_EQ(registry_.counters().reassignments, 1U);
    EXPECT_EQ(observer_.changes.back().first, quiet);
    EXPECT_EQ(observer_.changes.back().second, (Ownership{.node = self_, .generation = 6}));
}

TEST_F(RegistryTest, TheSweepLeavesOutARoomThatIsBeingLookedUp) {
    const core::RoomId looked_up = new_room();
    const core::RoomId quiet = new_room();
    registry_.set_interest(looked_up, true);
    registry_.set_interest(quiet, true);
    registry_.resolve(looked_up, [](const rt::StoreResult<Ownership>&) noexcept {});
    registry_.tick();
    ASSERT_EQ(store_.claims.size(), 1U);
    EXPECT_EQ(store_.claims.front().rooms, std::vector<core::RoomId>{quiet});
}

TEST_F(RegistryTest, NoSweepWithoutRoomsOfInterest) {
    registry_.tick();
    EXPECT_TRUE(store_.claims.empty());
    EXPECT_EQ(store_.heartbeats.size(), 1U);
}

TEST_F(RegistryTest, AResyncForgetsOtherOwnersAndLooksUpEveryRoomOfInterestAgain) {
    const core::RoomId watched = new_room();
    const core::RoomId mine = new_room();
    registry_.set_interest(watched, true);
    registry_.set_interest(mine, true);
    resolve_to(watched, {.node = other_, .generation = 1});
    own(mine, 1);

    store_.listener().on_resync();
    EXPECT_FALSE(registry_.known_owner(watched));
    EXPECT_EQ(registry_.owned(mine), 1U);
    ASSERT_EQ(store_.resolves.size(), 1U);
    EXPECT_EQ(store_.resolves.front().room, watched);
    FakeRoomStore::take(store_.resolves).done(Ownership{.node = other_, .generation = 2});
    EXPECT_EQ(registry_.known_owner(watched), (Ownership{.node = other_, .generation = 2}));
}

TEST_F(RegistryTest, ForgettingDropsAnotherNodesEntryButNeverThisNodesOwnership) {
    const core::RoomId theirs = new_room();
    const core::RoomId mine = new_room();
    resolve_to(theirs, {.node = other_, .generation = 1});
    own(mine, 1);
    registry_.forget(theirs);
    registry_.forget(mine);
    EXPECT_FALSE(registry_.known_owner(theirs));
    EXPECT_EQ(registry_.known_owner(mine), (Ownership{.node = self_, .generation = 1}));
}

TEST_F(RegistryTest, LosingInterestDropsTheCachedOwner) {
    const core::RoomId room = new_room();
    registry_.set_interest(room, true);
    resolve_to(room, {.node = other_, .generation = 1});
    registry_.set_interest(room, false);
    EXPECT_FALSE(registry_.known_owner(room));
}

TEST_F(RegistryTest, ReleasingOneRoomGivesUpThatRoomAlone) {
    const core::RoomId idle = new_room();
    const core::RoomId busy = new_room();
    own(idle, 3);
    own(busy, 1);
    registry_.release(idle);
    EXPECT_FALSE(registry_.owned(idle));
    EXPECT_FALSE(registry_.known_owner(idle));
    EXPECT_EQ(registry_.owned_rooms(), std::vector<core::RoomId>{busy});
    ASSERT_EQ(store_.releases.size(), 1U);
    EXPECT_EQ(store_.releases.front().rooms,
              (std::vector<OwnedRoom>{{.room = idle, .generation = 3}}));
    registry_.release(idle);
    EXPECT_EQ(store_.releases.size(), 1U);
}

TEST_F(RegistryTest, ReleasingForADrainGivesUpEveryRoomUnderItsGeneration) {
    const core::RoomId a = new_room();
    const core::RoomId b = new_room();
    own(a, 1);
    own(b, 7);
    bool released = false;
    registry_.release_all([&](rt::StoreResult<void> r) noexcept { released = r.has_value(); });
    EXPECT_EQ(registry_.rooms_owned(), 0U);
    EXPECT_FALSE(registry_.append(a, message_, [](auto) noexcept {}));
    ASSERT_EQ(store_.releases.size(), 1U);
    auto rooms = store_.releases.front().rooms;
    ASSERT_EQ(rooms.size(), 2U);
    std::ranges::sort(rooms, {}, &OwnedRoom::generation);
    EXPECT_EQ(rooms[0].room, a);
    EXPECT_EQ(rooms[1].room, b);
    EXPECT_EQ(rooms[1].generation, 7U);
    FakeRoomStore::take(store_.releases).done({});
    EXPECT_TRUE(released);
}

} // namespace
