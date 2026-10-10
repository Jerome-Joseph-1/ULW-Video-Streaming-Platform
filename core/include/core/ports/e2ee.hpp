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
    // No such device for this user. A device registered by someone else reads the same, to
    // every call registration included, so a caller cannot probe for other people's devices.
    NotFound,
    // The device was deregistered. Its id stays retired while its tombstone is kept.
    Revoked,
    // The device has no key package left, single-use or last-resort. This is the replenish
    // signal: whoever asked should tell the device to publish more, and try again after it has.
    Exhausted,
    // Publishing the batch would take the device above kMaxKeyPackagesPerDevice, or registering
    // the device would take its user above kMaxDevicesPerUser. Nothing was stored.
    Full,
    // A batch that is empty or holds more than kMaxKeyPackagesPerDevice, a package that is empty
    // or above kMaxKeyPackageBytes, or a commit that is empty or above kMaxCommitBytes: no call
    // could ever store it. Nothing was stored.
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

// Clients are browsers (ADR-0016), and every browser profile is a device. A phone, a tablet and
// a couple of computers with a few browsers each stay under 10; 16 leaves room for profiles
// wiped without deregistering, which the user retires from the device list. It also bounds what
// one user can make the directory hold: 16 * 800 KiB of key packages.
inline constexpr std::size_t kMaxDevicesPerUser = 16;

// A retired id is refused if it comes back, which stops a stale client from reviving a device
// its groups removed. Only the most recent retirements need that: a client that has been gone
// for 64 retirements has long since been removed everywhere. Older tombstones are dropped, so
// churning devices cannot grow a user's rows without bound.
inline constexpr std::size_t kRetiredDevicesKept = 64;

// Checks a batch against the size bounds before anything is stored: Invalid, or nothing.
[[nodiscard]] E2eeResult<void> check_key_package_batch(std::span<const KeyPackageBytes> batch);
// The same for one package on its own, a last-resort one.
[[nodiscard]] E2eeResult<void> check_key_package(const KeyPackageBytes& package);

// An MLS commit exactly as its sender serialised it. Like a key package, never parsed here.
using CommitBytes = std::vector<std::byte>;

// Chat groups are small (ADR-0016). A commit carries one HPKE ciphertext of about 80 bytes per
// node on its update path, 7 of them in a 128-member tree, plus a key package (about 300 bytes)
// per member it adds: adding 100 members at once stays near 31 KiB. 64 KiB is twice that.
inline constexpr std::size_t kMaxCommitBytes = std::size_t{64} * 1024;

// A member coming back after a long absence catches up a page at a time.
inline constexpr std::size_t kCommitPage = 32;

struct StoredCommit {
    std::uint64_t epoch = 0;
    CommitBytes commit;
};

struct FetchedKeyPackage {
    KeyPackageBytes package;
    // The device is at or below kKeyPackageLowWater and should be told to publish more. An
    // early warning only: fetches running side by side each count the packages the others are
    // taking, so a burst can cross the mark without any of them raising it. Exhausted, which
    // no burst can hide, remains the signal that must be acted on; so is a last-resort package.
    bool replenish = false;
    // The device's last-resort package (RFC 9420 section 16.8, ADR-0102): its single-use ones
    // had run out. It stays with the device and may be handed out again; `replenish` is set.
    bool last_resort = false;
};

// Where a device's last-resort package stands (ADR-0102).
enum class LastResort : std::uint8_t {
    // None published since the device was registered.
    None,
    // Published and never handed out.
    Fresh,
    // Handed out at least once since it was published: the device should publish another,
    // since every group that took it shares its init key until the device updates its leaf.
    Used,
};

// A live device of a user, as the registry lists it.
struct DeviceEntry {
    DeviceId device;
    // The single-use packages it holds.
    std::size_t key_packages = 0;
    LastResort last_resort = LastResort::None;
};

// The devices of each user. Every MLS member is one device (ADR-0016), so every key package
// and every commit names one.
class IDeviceRegistry {
public:
    virtual ~IDeviceRegistry() = default;

    // `device` is a UUIDv7 the client mints, so a registration replayed after its reply was
    // lost succeeds again without a second row. Full when the user already has
    // kMaxDevicesPerUser live devices.
    virtual void register_device(const UserId& user, const DeviceId& device,
                                 E2eeCallback<void> done) = 0;
    // One atomic step: the device is retired and every key package it published is gone; the
    // user's tombstones beyond the kRetiredDevicesKept most recent are dropped. A
    // fetch that has not returned by then gets Revoked; one that returned first got its package
    // before the device was retired.
    virtual void deregister_device(const UserId& user, const DeviceId& device,
                                   E2eeCallback<void> done) = 0;
    // The user's live devices, in byte order of their ids: at most kMaxDevicesPerUser. Retired
    // devices are not listed. A user with none, or one nobody has heard of, has an empty list.
    virtual void list_devices(const UserId& user, E2eeCallback<std::vector<DeviceEntry>> done) = 0;
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
    // Replaces the device's last-resort package (ADR-0102), which becomes Fresh. Invalid when
    // empty or above kMaxKeyPackageBytes.
    virtual void publish_last_resort(const UserId& user, const DeviceId& device,
                                     KeyPackageBytes package, E2eeCallback<void> done) = 0;
    // Takes one package of `user`'s `device` and deletes it: a package is single use, so of
    // any number of concurrent fetchers exactly one receives it. With none left it hands out the
    // device's last-resort package, which it keeps and marks Used, and only without one is the
    // answer Exhausted.
    virtual void fetch_key_package(const UserId& user, const DeviceId& device,
                                   E2eeCallback<FetchedKeyPackage> done) = 0;
    // Records `commit`, which `user`'s `committer` built at `epoch`, as the room's transition out
    // of that epoch. Epochs are taken in order from 0 and the first commit for an epoch wins;
    // every other is StaleEpoch, and its sender must process the winning commit and build its
    // change again on top. The claim and the commit are one row, so no accepted epoch can lack
    // its commit: a member that missed the room's copy fetches it with fetch_commits.
    virtual void submit_commit(const RoomId& room, const UserId& user, const DeviceId& committer,
                               std::uint64_t epoch, CommitBytes commit,
                               E2eeCallback<void> done) = 0;
    // The accepted commits of `room` from `from_epoch` on, oldest first, at most kCommitPage of
    // them; empty once the caller is current. Who may read a room is chat's decision.
    virtual void fetch_commits(const RoomId& room, std::uint64_t from_epoch,
                               E2eeCallback<std::vector<StoredCommit>> done) = 0;
};

} // namespace core::ports
