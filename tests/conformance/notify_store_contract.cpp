#include "notify_store_contract.hpp"

#include <algorithm>
#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace ulw::test {

namespace {

using core::ports::NotifyError;
using core::ports::RoomKind;
using core::ports::SocketDevice;

constexpr std::uint64_t kNodeA = 0x8000'0000'0000'0001;
constexpr std::uint64_t kNodeB = 2;

std::vector<SocketDevice> sorted(std::vector<SocketDevice> v) {
    std::ranges::sort(v, [](const SocketDevice& a, const SocketDevice& b) {
        if (a.user.view() != b.user.view()) {
            return a.user.view() < b.user.view();
        }
        if (a.device.has_value() != b.device.has_value()) {
            return !a.device.has_value();
        }
        return a.device && b.device && a.device->to_string() < b.device->to_string();
    });
    return v;
}

TEST_P(NotifyStoreContract, ARenewalSaysWhetherTheNodeWasNew) {
    // A tag past 2^63 is a negative bigint in the database: it must come back the same.
    EXPECT_EQ(renew(kNodeA), true);
    EXPECT_EQ(renew(kNodeA), false);
    EXPECT_EQ(renew(kNodeB), true);
}

TEST_P(NotifyStoreContract, ConnectedListsEachSocketsDeviceOnceAcrossNodes) {
    const core::DeviceId phone = new_device();
    const core::DeviceId laptop = new_device();
    ASSERT_TRUE(renew(kNodeA));
    ASSERT_TRUE(renew(kNodeB));
    ASSERT_TRUE(sync(kNodeA, {{.user = alice, .device = phone}, {.user = bob, .device = {}}}));
    ASSERT_TRUE(
        sync(kNodeB, {{.user = alice, .device = phone}, {.user = alice, .device = laptop}}));

    const auto alice_bob = connected({alice, bob, carol});
    ASSERT_TRUE(alice_bob) << to_string(alice_bob.error());
    EXPECT_EQ(sorted(*alice_bob), sorted({{.user = alice, .device = phone},
                                          {.user = alice, .device = laptop},
                                          {.user = bob, .device = {}}}));
    const auto nobody = connected({carol});
    ASSERT_TRUE(nobody);
    EXPECT_TRUE(nobody->empty());
}

TEST_P(NotifyStoreContract, RemovedSocketsNoLongerCountAndASyncCanBeSentAgain) {
    const core::DeviceId phone = new_device();
    ASSERT_TRUE(renew(kNodeA));
    ASSERT_TRUE(sync(kNodeA, {{.user = alice, .device = phone}}));
    // The same changes again, as after a lost answer: nothing changes.
    ASSERT_TRUE(sync(kNodeA, {{.user = alice, .device = phone}}));
    ASSERT_EQ(connected({alice})->size(), 1U);
    ASSERT_TRUE(sync(kNodeA, {}, {{.user = alice, .device = phone}}));
    ASSERT_TRUE(sync(kNodeA, {}, {{.user = alice, .device = phone}}));
    EXPECT_TRUE(connected({alice})->empty());
}

TEST_P(NotifyStoreContract, ASyncThatAddsAndRemovesTheSameSocketIsRefused) {
    ASSERT_TRUE(renew(kNodeA));
    const SocketDevice s{.user = alice, .device = new_device()};
    EXPECT_EQ(sync(kNodeA, {s}, {s}).error(), NotifyError::Unavailable);
    EXPECT_TRUE(connected({alice})->empty());
}

TEST_P(NotifyStoreContract, TooManyUsersAskedIsRefused) {
    std::vector<core::UserId> users;
    for (std::size_t i = 0; i <= core::ports::kMaxConnectedQuery; ++i) {
        users.push_back(*core::UserId::parse("user-" + std::to_string(i)));
    }
    EXPECT_EQ(connected(users).error(), NotifyError::Unavailable);
}

TEST_P(NotifyStoreContract, ANodePastItsLeaseCountsForNothingUntilItRenews) {
    ASSERT_TRUE(renew(kNodeA));
    ASSERT_TRUE(sync(kNodeA, {{.user = alice, .device = new_device()}}));
    harness_->wait(kContractTiming.lease + core::Millis{100});
    EXPECT_TRUE(connected({alice})->empty());
    // Back within forget_after: its rows were kept, and count again.
    EXPECT_EQ(renew(kNodeA), false);
    EXPECT_EQ(connected({alice})->size(), 1U);
}

TEST_P(NotifyStoreContract, ANodeForgottenByAnothersRenewalComesBackNewAndEmpty) {
    ASSERT_TRUE(renew(kNodeA));
    ASSERT_TRUE(sync(kNodeA, {{.user = alice, .device = new_device()}}));
    harness_->wait(kContractTiming.forget_after + core::Millis{200});
    ASSERT_TRUE(renew(kNodeB));
    EXPECT_EQ(renew(kNodeA), true);
    EXPECT_TRUE(connected({alice})->empty());
}

TEST_P(NotifyStoreContract, AForgottenNodesSocketsGoWithIt) {
    ASSERT_TRUE(renew(kNodeA));
    ASSERT_TRUE(sync(kNodeA, {{.user = alice, .device = new_device()}}));
    ASSERT_TRUE(forget(kNodeA));
    EXPECT_TRUE(connected({alice})->empty());
    EXPECT_EQ(renew(kNodeA), true);
}

TEST_P(NotifyStoreContract, AMessageIsOwedToMembersWithNoSocketOncePerInterval) {
    const core::RoomId room = new_room();
    harness_->set_room(room, RoomKind::GroupChat, {alice, bob, carol});
    ASSERT_TRUE(renew(kNodeA));
    // Carol has a socket, one that named no device: she is connected all the same.
    ASSERT_TRUE(sync(kNodeA, {{.user = carol, .device = {}}}));

    const auto first = targets(room, alice);
    ASSERT_TRUE(first) << to_string(first.error());
    EXPECT_EQ(*first, std::vector<core::UserId>{bob});
    // Within the interval: not bob again. Alice, the sender until now, is owed one when carol
    // writes; then nobody is.
    EXPECT_TRUE(targets(room, alice)->empty());
    EXPECT_EQ(*targets(room, carol), std::vector<core::UserId>{alice});
    EXPECT_TRUE(targets(room, carol)->empty());
    harness_->wait(kContractTiming.message_interval + core::Millis{100});
    ASSERT_TRUE(renew(kNodeA));
    // Past the interval, both are owed one again.
    EXPECT_EQ(*targets(room, carol), (std::vector<core::UserId>{alice, bob}));
}

TEST_P(NotifyStoreContract, AMemberWhoConnectsStartsAfresh) {
    const core::RoomId room = new_room();
    harness_->set_room(room, RoomKind::DirectChat, {alice, bob});
    ASSERT_TRUE(renew(kNodeA));
    EXPECT_EQ(*targets(room, alice), std::vector<core::UserId>{bob});
    const SocketDevice phone{.user = bob, .device = new_device()};
    ASSERT_TRUE(sync(kNodeA, {phone}));
    EXPECT_TRUE(targets(room, alice)->empty());
    ASSERT_TRUE(sync(kNodeA, {}, {phone}));
    // Gone again, within what would have been the interval: owed one at once.
    EXPECT_EQ(*targets(room, alice), std::vector<core::UserId>{bob});
}

TEST_P(NotifyStoreContract, MarksAreKeptPerRoom) {
    const core::RoomId one = new_room();
    const core::RoomId two = new_room();
    harness_->set_room(one, RoomKind::DirectChat, {alice, bob});
    harness_->set_room(two, RoomKind::GroupChat, {alice, bob, carol});
    ASSERT_TRUE(renew(kNodeA));
    EXPECT_EQ(*targets(one, alice), std::vector<core::UserId>{bob});
    EXPECT_EQ(*targets(two, alice), (std::vector<core::UserId>{bob, carol}));
}

TEST_P(NotifyStoreContract, AStreamsLiveChatAndAnUnknownRoomOweNobody) {
    // Shaped as a stream's chat is (version 8, tagged 0x01), which the database insists on.
    std::string text = new_room().to_string();
    text.replace(0, 2, "01");
    text[14] = '8';
    const core::RoomId live = *core::RoomId::parse(text);
    ASSERT_TRUE(core::ports::is_stream_chat(live));
    harness_->set_room(live, RoomKind::StreamLiveChat, {alice, bob});
    ASSERT_TRUE(renew(kNodeA));
    EXPECT_TRUE(targets(live, alice)->empty());
    EXPECT_TRUE(targets(new_room(), alice)->empty());
}

} // namespace

} // namespace ulw::test
