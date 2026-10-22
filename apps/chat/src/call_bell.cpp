#include "call_bell.hpp"

#include "envelope.hpp"
#include "presence_room.hpp"
#include "ring.hpp"
#include "room_notices.hpp"

#include <algorithm>
#include <new>

namespace chat {

BellId CallBell::attach(IClient& client, const core::UserId& user) {
    const std::uint64_t id = next_++;
    auto& sockets = users_[user];
    sockets.push_back({.id = id, .client = &client});
    try {
        sockets_.emplace(id, user);
    } catch (const std::bad_alloc&) {
        sockets.pop_back();
        if (sockets.empty()) {
            users_.erase(user);
        }
        throw;
    }
    if (store_ != nullptr) {
        store_->rings_for(
            user,
            [this, alive = std::weak_ptr(alive_), id, user](
                core::ports::RingStoreResult<std::vector<core::ports::StoredRing>> rings) noexcept {
                replay(alive, id, user, std::move(rings));
            });
    }
    return BellId{id};
}

void CallBell::replay(
    const std::weak_ptr<int>& alive, std::uint64_t id, const core::UserId& user,
    core::ports::RingStoreResult<std::vector<core::ports::StoredRing>> rings) noexcept {
    if (alive.expired()) {
        return;
    }
    if (!rings) {
        ++counters_.replay_failures;
        return;
    }
    const auto it = users_.find(user);
    if (it == users_.end()) {
        return;
    }
    const auto socket = std::ranges::find(it->second, id, &Socket::id);
    if (socket == it->second.end()) {
        return;
    }
    try {
        for (const core::ports::StoredRing& ring : *rings) {
            const auto call = CallId::parse(ring.call.to_string());
            if (!call) {
                continue;
            }
            text_.clear();
            write_call_event(text_, CallNotice{.event = RingEvent::Ringing,
                                               .to = user,
                                               .room = ring.room,
                                               .call = *call,
                                               .from = ring.caller,
                                               .by = std::nullopt,
                                               .expires_at = ring.expires_at});
            if (socket->client->push(text_)) {
                ++counters_.replayed;
            }
        }
    } catch (const std::bad_alloc&) {
        ++counters_.allocation_failures;
    }
}

void CallBell::detach(BellId id) noexcept {
    const auto it = sockets_.find(id.value);
    if (it == sockets_.end()) {
        return;
    }
    const auto user = users_.find(it->second);
    sockets_.erase(it);
    if (user == users_.end()) {
        return;
    }
    std::erase_if(user->second, [&](const Socket& s) { return s.id == id.value; });
    if (user->second.empty()) {
        users_.erase(user);
    }
}

void CallBell::activity(const core::RoomId& room, std::span<const std::byte> body) {
    const auto notice = decode_activity(body);
    // As for a ring: only the member's own presence room carries their room's activity.
    if (!notice || presence_room(notice->to) != room) {
        ++counters_.malformed;
        return;
    }
    const auto it = users_.find(notice->to);
    if (it == users_.end()) {
        ++counters_.unheard;
        return;
    }
    text_.clear();
    write_activity(text_, *notice);
    for (const Socket& s : it->second) {
        if (s.client->push(text_)) {
            ++counters_.activity;
        }
    }
}

void CallBell::on_notice(const core::RoomId& room, std::span<const std::byte> body) noexcept {
    if (rooms_ != nullptr && !is_presence_room(room)) {
        rooms_->on_notice(room, body);
        return;
    }
    try {
        if (is_activity(body)) {
            activity(room, body);
            return;
        }
        const auto notice = decode_notice(body);
        // Only the member's own presence room carries their ring: a notice in any other room
        // is not one the ring sent.
        if (!notice || presence_room(notice->to) != room) {
            ++counters_.malformed;
            return;
        }
        const auto it = users_.find(notice->to);
        if (it == users_.end()) {
            ++counters_.unheard;
            return;
        }
        text_.clear();
        write_call_event(text_, *notice);
        for (const Socket& s : it->second) {
            if (s.client->push(text_)) {
                ++counters_.pushed;
            }
        }
    } catch (const std::bad_alloc&) {
        ++counters_.allocation_failures;
    }
}

} // namespace chat
