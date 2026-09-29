#pragma once

#include "core/models/ids.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace core::ports {

enum class E2eeError : std::uint8_t {
    // No such device for this user. A device registered by someone else reads the same, so a
    // caller cannot probe for other people's devices.
    NotFound,
    // The device was deregistered. Its id stays retired: it can never be registered again.
    Revoked,
    // The device id is registered to another user.
    Conflict,
    // The device has no key package left. This is the replenish signal: whoever asked should
    // tell the device to publish more, and try again after it has.
    Exhausted,
    // Publishing the batch would take the device above kMaxKeyPackagesPerDevice. Nothing was
    // stored.
    Full,
    // A batch that is empty or holds more than kMaxKeyPackagesPerDevice, or a package that is
    // empty or above kMaxKeyPackageBytes: no device could ever store it. Nothing was stored.
    Invalid,
    // Another commit already moved the room out of this epoch, or the room is not at it yet.
    StaleEpoch,
    // The database is unreachable or timed out; the call may be retried.
    Unavailable,
    // A stored row violates an invariant.
    Corrupt,
};

[[nodiscard]] std::string_view to_string(E2eeError e) noexcept;

template <class T> using E2eeResult = std::expected<T, E2eeError>;

// Called exactly once, on the reactor thread, never from inside the call that was given it.
template <class T> using E2eeCallback = std::move_only_function<void(E2eeResult<T>) noexcept>;

// An MLS KeyPackage exactly as the client serialised it. The server never parses one.
using KeyPackageBytes = std::vector<std::byte>;

// A package for the mandatory ciphersuite with a basic credential is about 300 bytes. 8 KiB
// leaves room for a hybrid post-quantum init key (X-Wing's public key alone is 1216 bytes) and
// an X.509 credential chain, and bounds what one device can make the directory hold.
inline constexpr std::size_t kMaxKeyPackageBytes = 8192;

// Every invitation into a group spends one package, and a device can replenish only while it is
// online. 100 carries a device through a hundred new conversations started while it was away,
// and bounds its share of the table at 100 * 8 KiB = 800 KiB.
inline constexpr std::size_t kMaxKeyPackagesPerDevice = 100;

// The replenish signal is raised early, while a fifth of the supply is left: it can only be
// delivered once the device is back online, and the remaining packages cover the invitations
// that arrive in the meantime.
inline constexpr std::size_t kKeyPackageLowWater = kMaxKeyPackagesPerDevice / 5;

// Checks a batch against the size bounds before anything is stored: Invalid, or nothing.
[[nodiscard]] E2eeResult<void> check_key_package_batch(std::span<const KeyPackageBytes> batch);

struct FetchedKeyPackage {
    KeyPackageBytes package;
    // The device is at or below kKeyPackageLowWater and should be told to publish more.
    bool replenish = false;
};

// The devices of each user. Every MLS member is one device (ADR-0016), so every key package
// and every commit names one.
class IDeviceRegistry {
public:
    virtual ~IDeviceRegistry() = default;

    // `device` is a UUIDv7 the client mints, so a registration replayed after its reply was
    // lost succeeds again without a second row.
    virtual void register_device(const UserId& user, const DeviceId& device,
                                 E2eeCallback<void> done) = 0;
    // One atomic step: the device is retired and every key package it published is gone. A
    // fetch that has not returned by then gets Revoked; one that returned first got its package
    // before the device was retired.
    virtual void deregister_device(const UserId& user, const DeviceId& device,
                                   E2eeCallback<void> done) = 0;
};

// What the server does for MLS (RFC 9420): it hands out key packages and orders commits. It
// moves opaque bytes and holds no key material.
class IE2eeDeliveryService {
public:
    virtual ~IE2eeDeliveryService() = default;

    // All or nothing. Yields how many packages the device holds afterwards.
    virtual void publish_key_packages(const UserId& user, const DeviceId& device,
                                      std::vector<KeyPackageBytes> batch,
                                      E2eeCallback<std::size_t> done) = 0;
    // Takes one package of `user`'s `device` and deletes it: a package is single use, so of
    // any number of concurrent fetchers exactly one receives it.
    virtual void fetch_key_package(const UserId& user, const DeviceId& device,
                                   E2eeCallback<FetchedKeyPackage> done) = 0;
    // Claims the transition out of `epoch` in `room` for a commit that `user`'s `committer`
    // built at that epoch. Epochs are claimed in order from 0, and the first claim for an epoch
    // wins; every other is StaleEpoch, and its sender must process the winning commit and build
    // its change again on top. The commit itself reaches the members through the room as an
    // ordinary opaque message once its claim is accepted, so the server never reads it.
    virtual void submit_commit(const RoomId& room, const UserId& user, const DeviceId& committer,
                               std::uint64_t epoch, E2eeCallback<void> done) = 0;
};

} // namespace core::ports
