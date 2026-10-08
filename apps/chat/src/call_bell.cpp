#include "call_bell.hpp"

#include "envelope.hpp"
#include "presence_room.hpp"
#include "ring.hpp"

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
    return BellId{id};
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

void CallBell::on_notice(const core::RoomId& room, std::span<const std::byte> body) noexcept {
    try {
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
