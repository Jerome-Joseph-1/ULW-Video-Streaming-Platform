#pragma once

#include "core/models/ids.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <vector>

namespace core::ports {

// A browser's Web Push subscription for one device of a user (docs/adr/0097): where its push
// service takes messages for it, and the keys a message to it is encrypted with (RFC 8291). The
// keys are public by design; the endpoint is a capability (whoever holds it, with the keys, can
// make that device show a notification), so it is never logged.
struct PushSubscription {
    DeviceId device;
    std::string endpoint;
    // The browser's P-256 key, uncompressed.
    std::array<std::uint8_t, 65> p256dh{};
    std::array<std::uint8_t, 16> auth{};
};

enum class PushStoreError : std::uint8_t {
    // Unreachable, timed out, or lost a race with a concurrent write; the call may be repeated.
    Unavailable,
    // A row this code could not have written.
    Corrupt,
};

template <class T> using PushResult = std::expected<T, PushStoreError>;
// Called exactly once, on the reactor thread, never from inside the call that was given it.
template <class T> using PushCallback = std::move_only_function<void(PushResult<T>) noexcept>;

class IPushSubscriptions {
public:
    virtual ~IPushSubscriptions() = default;

    // Keeps `subscription` as the user's device's, replacing what that device had. An endpoint
    // belongs to one device of one user: whoever held it before (another account signed in on
    // the same browser) loses it. Past `max_per_user` devices, the ones updated longest ago are
    // forgotten.
    virtual void save(const UserId& user, const PushSubscription& subscription,
                      std::size_t max_per_user, PushCallback<void> done) = 0;
    // Forgets the device's subscription; done whether or not it had one.
    virtual void remove(const UserId& user, const DeviceId& device, PushCallback<void> done) = 0;
    // The user's subscriptions, at most `limit`, most recently updated first.
    virtual void list(const UserId& user, std::size_t limit,
                      PushCallback<std::vector<PushSubscription>> done) = 0;
    // Forgets the subscription at `endpoint`, whoever's it is: its push service said it is gone.
    virtual void forget(const std::string& endpoint, PushCallback<void> done) = 0;
};

} // namespace core::ports
