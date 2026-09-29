#include "e2ee_directory_contract.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <set>
#include <vector>

namespace ulw::test {

core::ports::KeyPackageBytes package_of(std::size_t size, std::uint8_t seed) {
    core::ports::KeyPackageBytes out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::byte>((seed + i) & 0xFFU);
    }
    return out;
}

core::ports::CommitBytes commit_body(std::uint64_t epoch, std::uint8_t author) {
    // Epochs in tests stay far below 256.
    return package_of(120, static_cast<std::uint8_t>((epoch * 16) + author));
}

namespace {

using core::ports::E2eeError;
using core::ports::E2eeResult;
using core::ports::KeyPackageBytes;
using core::ports::kKeyPackageLowWater;
using core::ports::kMaxKeyPackageBytes;
using core::ports::kMaxKeyPackagesPerDevice;

TEST_P(DirectoryContract, FetchesHandOutPublishedPackagesOldestFirstThenExhaust) {
    const core::DeviceId device = new_device();
    ASSERT_TRUE(enrol(alice, device));
    ASSERT_EQ(publish(alice, device, {package_of(300, 1), package_of(310, 2)}), 2U);

    const auto first = fetch(alice, device);
    ASSERT_TRUE(first) << to_string(first.error());
    EXPECT_EQ(first->package, package_of(300, 1));
    const auto second = fetch(alice, device);
    ASSERT_TRUE(second) << to_string(second.error());
    EXPECT_EQ(second->package, package_of(310, 2));
    EXPECT_EQ(fetch(alice, device).error(), E2eeError::Exhausted);
}

TEST_P(DirectoryContract, PackagesRoundTripByteForByte) {
    const core::DeviceId device = new_device();
    ASSERT_TRUE(enrol(alice, device));
    // Every byte value, NUL and the backslash included, at the largest size allowed.
    const KeyPackageBytes package = package_of(kMaxKeyPackageBytes, 0);
    ASSERT_TRUE(publish(alice, device, {package}));
    const auto fetched = fetch(alice, device);
    ASSERT_TRUE(fetched);
    EXPECT_EQ(fetched->package, package);
}

TEST_P(DirectoryContract, EachPackageIsHandedOutOnce) {
    const core::DeviceId device = stocked_device(5);
    std::set<KeyPackageBytes> seen;
    for (int i = 0; i < 5; ++i) {
        const auto fetched = fetch(alice, device);
        ASSERT_TRUE(fetched);
        EXPECT_TRUE(seen.insert(fetched->package).second) << "package handed out twice";
    }
    EXPECT_EQ(fetch(alice, device).error(), E2eeError::Exhausted);
}

TEST_P(DirectoryContract, ExhaustedDeviceServesAgainOnceReplenished) {
    const core::DeviceId device = stocked_device(1);
    const auto last = fetch(alice, device);
    ASSERT_TRUE(last);
    EXPECT_TRUE(last->replenish);
    ASSERT_EQ(fetch(alice, device).error(), E2eeError::Exhausted);

    ASSERT_EQ(publish(alice, device, {package_of(300, 9)}), 1U);
    const auto fresh = fetch(alice, device);
    ASSERT_TRUE(fresh);
    EXPECT_EQ(fresh->package, package_of(300, 9));
}

TEST_P(DirectoryContract, ReplenishRisesWhenTheSupplyReachesTheLowWaterMark) {
    const core::DeviceId device = stocked_device(kKeyPackageLowWater + 2);
    const auto above = fetch(alice, device);
    ASSERT_TRUE(above);
    EXPECT_FALSE(above->replenish) << "raised with " << kKeyPackageLowWater + 1 << " left";
    const auto at = fetch(alice, device);
    ASSERT_TRUE(at);
    EXPECT_TRUE(at->replenish) << "not raised with " << kKeyPackageLowWater << " left";
}

TEST_P(DirectoryContract, PublishReportsTheSupplyHeld) {
    const core::DeviceId device = stocked_device(3);
    ASSERT_TRUE(fetch(alice, device));
    EXPECT_EQ(publish(alice, device, {package_of(300, 7), package_of(300, 8)}), 4U);
}

TEST_P(DirectoryContract, PublishBeyondTheCapIsRefusedWhole) {
    const core::DeviceId device = stocked_device(kMaxKeyPackagesPerDevice - 1);
    EXPECT_EQ(publish(alice, device, {package_of(300, 1), package_of(300, 2)}).error(),
              E2eeError::Full);
    EXPECT_EQ(publish(alice, device, {package_of(300, 3)}), kMaxKeyPackagesPerDevice);
    EXPECT_EQ(publish(alice, device, {package_of(300, 4)}).error(), E2eeError::Full);
}

TEST_P(DirectoryContract, BatchWithAnyBadPackageStoresNothing) {
    const core::DeviceId device = new_device();
    ASSERT_TRUE(enrol(alice, device));
    EXPECT_EQ(publish(alice, device, {package_of(300, 1), package_of(kMaxKeyPackageBytes + 1, 2)})
                  .error(),
              E2eeError::Invalid);
    EXPECT_EQ(publish(alice, device, {package_of(300, 1), KeyPackageBytes{}}).error(),
              E2eeError::Invalid);
    EXPECT_EQ(publish(alice, device, {}).error(), E2eeError::Invalid);
    EXPECT_EQ(fetch(alice, device).error(), E2eeError::Exhausted);
}

TEST_P(DirectoryContract, AnswersNeverArriveInsideTheCall) {
    const core::DeviceId device = new_device();
    Answer<void> enrolled;
    registry().register_device(alice, device, enrolled.callback());
    EXPECT_FALSE(enrolled.ready());
    // Refused before any statement could be sent, and still not from inside the call.
    Answer<std::size_t> refused;
    delivery().publish_key_packages(alice, device, {}, refused.callback());
    EXPECT_FALSE(refused.ready());
    EXPECT_TRUE(await(reactor(), enrolled));
    EXPECT_EQ(await(reactor(), refused).error(), E2eeError::Invalid);
    EXPECT_EQ(refused.calls(), 1);
}

TEST_P(DirectoryContract, ReplayedRegistrationSucceeds) {
    const core::DeviceId device = new_device();
    ASSERT_TRUE(enrol(alice, device));
    EXPECT_TRUE(enrol(alice, device));
}

TEST_P(DirectoryContract, UsersLiveDevicesAreCapped) {
    std::vector<core::DeviceId> devices;
    for (std::size_t i = 0; i < core::ports::kMaxDevicesPerUser; ++i) {
        devices.push_back(new_device());
        ASSERT_TRUE(enrol(alice, devices.back())) << i;
    }
    const core::DeviceId extra = new_device();
    EXPECT_EQ(enrol(alice, extra).error(), E2eeError::Full);
    EXPECT_TRUE(enrol(alice, devices.front())) << "a replay at the cap still succeeds";
    EXPECT_TRUE(enrol(bob, new_device())) << "the cap is per user";
    ASSERT_TRUE(retire(alice, devices.front()));
    EXPECT_TRUE(enrol(alice, extra)) << "retiring a device frees its place";
}

TEST_P(DirectoryContract, OnlyTheNewestTombstonesAreKept) {
    std::vector<core::DeviceId> retired;
    for (std::size_t i = 0; i < core::ports::kRetiredDevicesKept + 2; ++i) {
        retired.push_back(new_device());
        ASSERT_TRUE(enrol(alice, retired.back()));
        ASSERT_TRUE(retire(alice, retired.back()));
    }
    // The two oldest tombstones were dropped, so their ids read as new; the rest stay retired.
    EXPECT_TRUE(enrol(alice, retired[0]));
    EXPECT_TRUE(enrol(alice, retired[1]));
    EXPECT_EQ(enrol(alice, retired[2]).error(), E2eeError::Revoked);
    EXPECT_EQ(enrol(alice, retired.back()).error(), E2eeError::Revoked);
}

TEST_P(DirectoryContract, AnotherUsersDeviceIsOutOfReach) {
    const core::DeviceId device = stocked_device(2);
    EXPECT_EQ(enrol(bob, device).error(), E2eeError::NotFound)
        << "registering must not reveal that the id is taken";
    EXPECT_EQ(fetch(bob, device).error(), E2eeError::NotFound);
    EXPECT_EQ(publish(bob, device, {package_of(300, 1)}).error(), E2eeError::NotFound);
    EXPECT_EQ(retire(bob, device).error(), E2eeError::NotFound);
    EXPECT_EQ(commit(new_room(), bob, device, 0).error(), E2eeError::NotFound);
    // None of it touched alice's supply.
    EXPECT_TRUE(fetch(alice, device));
    EXPECT_TRUE(fetch(alice, device));
}

TEST_P(DirectoryContract, UnknownDeviceIsNotFound) {
    const core::DeviceId device = new_device();
    EXPECT_EQ(fetch(alice, device).error(), E2eeError::NotFound);
    EXPECT_EQ(publish(alice, device, {package_of(300, 1)}).error(), E2eeError::NotFound);
    EXPECT_EQ(retire(alice, device).error(), E2eeError::NotFound);
}

TEST_P(DirectoryContract, DeregistrationRejectsFetchesAndDropsThePackages) {
    const core::DeviceId device = stocked_device(3);
    ASSERT_TRUE(retire(alice, device));
    EXPECT_EQ(fetch(alice, device).error(), E2eeError::Revoked);
    EXPECT_EQ(publish(alice, device, {package_of(300, 1)}).error(), E2eeError::Revoked);
    EXPECT_EQ(commit(new_room(), alice, device, 0).error(), E2eeError::Revoked);
    // Retired for good: the id cannot be registered again to bring the packages back.
    EXPECT_EQ(enrol(alice, device).error(), E2eeError::Revoked);
    EXPECT_EQ(fetch(alice, device).error(), E2eeError::Revoked);
    EXPECT_TRUE(retire(alice, device)) << "a replayed deregistration must succeed";
}

TEST_P(DirectoryContract, DeregistrationLeavesTheUsersOtherDevicesAlone) {
    const core::DeviceId phone = stocked_device(1);
    const core::DeviceId laptop = stocked_device(1);
    ASSERT_TRUE(retire(alice, phone));
    EXPECT_TRUE(fetch(alice, laptop));
}

TEST_P(DirectoryContract, CommitEpochsAreClaimedInOrderAndOnce) {
    const core::DeviceId phone = stocked_device(0);
    const core::DeviceId laptop = stocked_device(0);
    const core::RoomId room = new_room();
    EXPECT_EQ(commit(room, alice, phone, 1).error(), E2eeError::StaleEpoch)
        << "the room has not reached epoch 1";
    ASSERT_TRUE(commit(room, alice, phone, 0));
    EXPECT_EQ(commit(room, alice, laptop, 0).error(), E2eeError::StaleEpoch);
    EXPECT_EQ(commit(room, alice, laptop, 2).error(), E2eeError::StaleEpoch);
    EXPECT_TRUE(commit(room, alice, laptop, 1));
    EXPECT_TRUE(commit(new_room(), alice, laptop, 0)) << "rooms count epochs separately";
}

TEST_P(DirectoryContract, AcceptedCommitsAreKeptForMembersToFetch) {
    const core::DeviceId phone = stocked_device(0);
    const core::DeviceId laptop = stocked_device(0);
    const core::RoomId room = new_room();
    ASSERT_TRUE(commit(room, alice, phone, 0, 1));
    ASSERT_EQ(commit(room, alice, laptop, 0, 2).error(), E2eeError::StaleEpoch);
    ASSERT_TRUE(commit(room, alice, laptop, 1, 2));
    ASSERT_TRUE(commit(room, alice, phone, 2, 1));

    const auto all = commits(room, 0);
    ASSERT_TRUE(all);
    ASSERT_EQ(all->size(), 3U);
    // The winner's body for each epoch; the refused commit left nothing behind.
    for (std::uint64_t e = 0; e < 3; ++e) {
        EXPECT_EQ((*all)[e].epoch, e);
    }
    EXPECT_EQ((*all)[0].commit, commit_body(0, 1));
    EXPECT_EQ((*all)[1].commit, commit_body(1, 2));
    EXPECT_EQ((*all)[2].commit, commit_body(2, 1));

    const auto tail = commits(room, 2);
    ASSERT_TRUE(tail);
    ASSERT_EQ(tail->size(), 1U);
    EXPECT_EQ(tail->front().epoch, 2U);
    EXPECT_TRUE(commits(room, 3)->empty()) << "a current member has nothing to fetch";
    EXPECT_TRUE(commits(new_room(), 0)->empty());
}

TEST_P(DirectoryContract, CommitsComeAPageAtATime) {
    const core::DeviceId phone = stocked_device(0);
    const core::RoomId room = new_room();
    const std::uint64_t total = core::ports::kCommitPage + 3;
    for (std::uint64_t e = 0; e < total; ++e) {
        ASSERT_TRUE(commit(room, alice, phone, e));
    }
    const auto first = commits(room, 0);
    ASSERT_TRUE(first);
    ASSERT_EQ(first->size(), core::ports::kCommitPage);
    const auto rest = commits(room, first->back().epoch + 1);
    ASSERT_TRUE(rest);
    ASSERT_EQ(rest->size(), 3U);
    EXPECT_EQ(rest->back().epoch, total - 1);
}

TEST_P(DirectoryContract, EmptyOrOversizedCommitIsInvalid) {
    const core::DeviceId phone = stocked_device(0);
    const core::RoomId room = new_room();
    EXPECT_EQ(commit_bytes(room, alice, phone, 0, {}).error(), E2eeError::Invalid);
    EXPECT_EQ(commit_bytes(room, alice, phone, 0,
                           core::ports::CommitBytes(core::ports::kMaxCommitBytes + 1))
                  .error(),
              E2eeError::Invalid);
    EXPECT_TRUE(commit_bytes(room, alice, phone, 0,
                             core::ports::CommitBytes(core::ports::kMaxCommitBytes)));
}

} // namespace

} // namespace ulw::test
