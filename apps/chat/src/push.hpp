#pragma once

#include "core/models/ids.hpp"
#include "core/ports/clock.hpp"
#include "core/ports/push_subscriptions.hpp"
#include "infra/webpush/endpoint.hpp"
#include "infra/webpush/sender.hpp"
#include "infra/webpush/vapid.hpp"

#include "chat_service.hpp"
#include "envelope.hpp"
#include "ring.hpp"

#include <cstddef>
#include <cstdint>

// Web Push for calls (ADR-0097): clients register a browser's push subscription per device over
// the chat socket, and when a call starts ringing on the room's owner, every subscribed device
// of each callee gets an encrypted push (RFC 8291) through its browser's push service, signed
// with the operator's VAPID key (RFC 8292). The push carries what the socket's call_ringing
// carries, nothing of any message. Everything runs on the reactor thread.
namespace chat {

struct PushLimits {
    // Devices a user may have subscribed; a new one past it replaces the one updated longest ago.
    // A phone, a laptop, a work machine, a few browser profiles.
    // ULW_PUSH_MAX_SUBSCRIPTIONS_PER_USER.
    std::size_t max_per_user = 10;
    // Subscribes and unsubscribes waiting for the store, node-wide; past it one is answered busy.
    std::size_t max_writes = 64;
    // Callees' subscription reads waiting for the store, node-wide; a ring past it pushes to
    // nobody (counted). A ring reads one list per callee: 256 is a burst of rings far beyond a
    // node's.
    std::size_t max_lookups = 256;
};

// What a push needs that main.cpp makes: the store, the sender on its own libcurl multi, the
// VAPID key and the operator's allowlist of push service hosts.
struct PushDeps {
    core::ports::IPushSubscriptions& store;
    infra::webpush::PushSender& sender;
    const infra::webpush::VapidKey& key;
    infra::webpush::PushHosts hosts;
    PushLimits limits;
    // Endpoints on any port, for a test push service (ULW_DEV_PUSH_ALLOW_PRIVATE).
    bool dev_any_port = false;
};

struct PushCounters {
    std::uint64_t subscribed = 0;
    std::uint64_t unsubscribed = 0;
    // Subscribes refused for their endpoint or key.
    std::uint64_t refused = 0;
    // Subscribes and unsubscribes answered busy: max_writes were waiting.
    std::uint64_t busy = 0;
    // Store calls that failed: writes, lists and forgets.
    std::uint64_t store_failures = 0;
    // Subscriptions deleted because their push service said they are gone.
    std::uint64_t forgotten = 0;
    // Callees' subscription lists read for a ring, and those not read for want of a place.
    std::uint64_t lookups = 0;
    std::uint64_t lookups_dropped = 0;
    // Pushes handed to the sender.
    std::uint64_t messages = 0;
    // Stored subscriptions skipped: the endpoint is no longer one the allowlist names, or the
    // message could not be encrypted to its key.
    std::uint64_t skipped = 0;
};

class Push final : public IRingPush {
public:
    // `chat` is how answers find their client again; it, the store and the sender must outlive
    // this, and the store must drop what it owes once this is gone (the server's Services destroy
    // it first). The sender's gone endpoints come here until this is destroyed.
    Push(PushDeps deps, IClientLookup& chat, const core::ports::IClock& clock);
    ~Push() override;
    Push(const Push&) = delete;
    Push& operator=(const Push&) = delete;
    Push(Push&&) = delete;
    Push& operator=(Push&&) = delete;

    // The applicationServerKey, answered at once.
    void key(IClient& client);
    void subscribe(ClientId id, const core::UserId& user, const PushSubscribe& subscribe);
    void unsubscribe(ClientId id, const core::UserId& user, const PushUnsubscribe& unsubscribe);

    void ringing(const CallPush& push) noexcept override;
    // Runs the sender's retries. After every turn of the loop.
    void tick() noexcept { deps_.sender.tick(); }

    [[nodiscard]] const PushCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] const infra::webpush::SenderCounters& sender_counters() const noexcept {
        return deps_.sender.counters();
    }
    [[nodiscard]] std::size_t queued() const noexcept { return deps_.sender.queued(); }
    [[nodiscard]] std::size_t in_flight() const noexcept { return deps_.sender.in_flight(); }

private:
    void listed(const CallPush& push, const core::UserId& callee,
                core::ports::PushResult<std::vector<core::ports::PushSubscription>> list) noexcept;
    void answer(ClientId id, std::string_view type, const core::DeviceId& device,
                core::ports::PushResult<void> result) noexcept;
    void gone(const std::string& endpoint) noexcept;

    PushDeps deps_;
    IClientLookup& chat_;
    const core::ports::IClock& clock_;
    PushCounters counters_;
    std::size_t writes_ = 0;
    std::size_t lookups_ = 0;
};

} // namespace chat
