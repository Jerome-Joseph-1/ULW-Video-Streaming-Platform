#include "push.hpp"

#include "infra/webpush/ece.hpp"

#include <algorithm>
#include <new>
#include <span>
#include <string>
#include <utility>

namespace chat {

namespace {

using infra::webpush::EndpointError;

// Every ring's plaintext is padded to a multiple of this, so a push service cannot tell callers
// or rooms apart by the message's length: the event is about 200 bytes, a caller's id up to 128.
constexpr std::size_t kPadBucket = 512;

std::string_view endpoint_reason(EndpointError error) noexcept {
    switch (error) {
    case EndpointError::HostNotAllowed:
        return "push_host_not_allowed";
    case EndpointError::TooLong:
    case EndpointError::NotHttps:
    case EndpointError::Malformed:
    case EndpointError::AddressLiteral:
        return "bad_endpoint";
    }
    return "bad_endpoint";
}

} // namespace

Push::Push(PushDeps deps, IClientLookup& chat, const core::ports::IClock& clock)
    : deps_(std::move(deps)), chat_(chat), clock_(clock) {
    deps_.sender.on_gone([this](const std::string& endpoint) noexcept { gone(endpoint); });
}

Push::~Push() {
    deps_.sender.on_gone(nullptr);
}

void Push::key(IClient& client) {
    std::string out;
    write_push_key(out, deps_.key.public_key());
    client.push(out);
}

bool Push::may_write(const core::UserId& user) const noexcept {
    if (writes_ >= deps_.limits.max_writes) {
        return false;
    }
    const auto it = user_writes_.find(user);
    return it == user_writes_.end() || it->second < deps_.limits.max_writes_per_user;
}

template <class Submit> void Push::write(const core::UserId& user, Submit submit) {
    // The user's entry before the store hears of anything, so that once it has, counting cannot
    // fail; and counted only once the store took the call, so that a call that throws leaves
    // nothing counted that no answer would take back. The answer never comes from inside it.
    std::size_t& mine = user_writes_[user];
    try {
        submit();
    } catch (...) {
        if (mine == 0) {
            user_writes_.erase(user);
        }
        throw;
    }
    ++mine;
    ++writes_;
}

void Push::answer(ClientId id, const core::UserId& user, std::string_view type,
                  const core::DeviceId& device, core::ports::PushResult<void> result) noexcept {
    --writes_;
    if (const auto it = user_writes_.find(user); it != user_writes_.end() && --it->second == 0) {
        user_writes_.erase(it);
    }
    if (!result) {
        ++counters_.store_failures;
    } else if (type == "push_subscribed") {
        ++counters_.subscribed;
    } else {
        ++counters_.unsubscribed;
    }
    IClient* client = chat_.client(id);
    if (client == nullptr) {
        return;
    }
    try {
        std::string out;
        if (result) {
            write_push_done(out, type, device);
        } else {
            write_push_error(out, "unavailable", device);
        }
        client->push(out);
    } catch (const std::bad_alloc&) {
        client->allocation_failed();
    }
}

void Push::subscribe(ClientId id, const core::UserId& user, const PushSubscribe& subscribe) {
    IClient* client = chat_.client(id);
    if (client == nullptr) {
        return;
    }
    const auto refuse = [&](std::string_view reason) {
        std::string out;
        write_push_error(out, reason, subscribe.device);
        client->push(out);
    };
    auto endpoint =
        infra::webpush::check_endpoint(subscribe.endpoint, deps_.hosts, deps_.dev_any_port);
    if (!endpoint) {
        ++counters_.refused;
        refuse(endpoint_reason(endpoint.error()));
        return;
    }
    if (!infra::webpush::is_p256_point(subscribe.p256dh)) {
        ++counters_.refused;
        refuse("bad_key");
        return;
    }
    if (!may_write(user)) {
        ++counters_.busy;
        refuse("busy");
        return;
    }
    write(user, [&] {
        deps_.store.save(user,
                         core::ports::PushSubscription{.device = subscribe.device,
                                                       .endpoint = std::move(endpoint->url),
                                                       .p256dh = subscribe.p256dh,
                                                       .auth = subscribe.auth},
                         deps_.limits.max_per_user,
                         [this, id, user, device = subscribe.device](
                             core::ports::PushResult<void> result) noexcept {
                             answer(id, user, "push_subscribed", device, result);
                         });
    });
}

void Push::unsubscribe(ClientId id, const core::UserId& user, const PushUnsubscribe& unsubscribe) {
    IClient* client = chat_.client(id);
    if (client == nullptr) {
        return;
    }
    if (!may_write(user)) {
        ++counters_.busy;
        std::string out;
        write_push_error(out, "busy", unsubscribe.device);
        client->push(out);
        return;
    }
    write(user, [&] {
        deps_.store.remove(user, unsubscribe.device,
                           [this, id, user, device = unsubscribe.device](
                               core::ports::PushResult<void> result) noexcept {
                               answer(id, user, "push_unsubscribed", device, result);
                           });
    });
}

void Push::ringing(const CallPush& push) noexcept {
    for (const core::UserId& callee : push.callees) {
        if (lookups_ >= deps_.limits.max_lookups) {
            ++counters_.lookups_dropped;
            continue;
        }
        try {
            ++lookups_;
            ++counters_.lookups;
            deps_.store.list(callee, deps_.limits.max_per_user,
                             [this, push, callee](
                                 core::ports::PushResult<std::vector<core::ports::PushSubscription>>
                                     list) noexcept { listed(push, callee, std::move(list)); });
        } catch (const std::bad_alloc&) {
            --lookups_;
            ++counters_.lookups_dropped;
        }
    }
}

void Push::listed(
    const CallPush& push, const core::UserId& callee,
    core::ports::PushResult<std::vector<core::ports::PushSubscription>> list) noexcept {
    --lookups_;
    if (!list) {
        ++counters_.store_failures;
        return;
    }
    // The ring may have ended while the list was read; a push now would ring for nothing.
    if (clock_.now() >= push.deadline) {
        return;
    }
    try {
        // What the callee's sockets hear, word for word: a client handles both alike.
        std::string payload;
        write_call_event(payload, CallNotice{.event = RingEvent::Ringing,
                                             .to = callee,
                                             .room = push.room,
                                             .call = push.call,
                                             .from = push.from,
                                             .by = std::nullopt,
                                             .expires_at = push.expires_at});
        // The text's bytes, as octets; char and unsigned char may alias each other.
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
        const std::span<const std::uint8_t> plaintext{
            reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()};
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
        for (const core::ports::PushSubscription& s : *list) {
            // The allowlist may have narrowed since the subscription was saved.
            auto endpoint =
                infra::webpush::check_endpoint(s.endpoint, deps_.hosts, deps_.dev_any_port);
            if (!endpoint) {
                ++counters_.skipped;
                continue;
            }
            auto body = infra::webpush::encrypt(s.p256dh, s.auth, plaintext, kPadBucket);
            if (!body) {
                ++counters_.skipped;
                continue;
            }
            ++counters_.messages;
            // A full queue is counted by the sender.
            static_cast<void>(deps_.sender.enqueue(
                infra::webpush::PushMessage{.endpoint = std::move(endpoint->url),
                                            .audience = std::move(endpoint->origin),
                                            .body = std::move(*body),
                                            .deadline = push.deadline,
                                            .urgency = infra::webpush::Urgency::High}));
        }
    } catch (const std::bad_alloc&) {
        ++counters_.skipped;
    }
}

void Push::gone(const std::string& endpoint) noexcept {
    try {
        deps_.store.forget(endpoint, [this](core::ports::PushResult<void> result) noexcept {
            if (result) {
                ++counters_.forgotten;
            } else {
                ++counters_.store_failures;
            }
        });
    } catch (const std::bad_alloc&) {
        ++counters_.store_failures;
    }
}

} // namespace chat
