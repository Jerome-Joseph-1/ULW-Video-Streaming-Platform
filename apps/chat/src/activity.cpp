// A room's activity (ADR-0137): the node a message was sent from tells each of the room's members
// that the room moved on, through the member's presence room, which every node with a socket of
// theirs is in, so that a socket hears of rooms it has not joined. At most once a room each
// ServiceLimits::activity_interval from this node; what comes in between is folded into the next.
#include "chat_service.hpp"
#include "presence_room.hpp"

#include <algorithm>
#include <new>
#include <utility>

namespace chat {

void ChatService::note_activity(const core::RoomId& room, const core::UserId& sender,
                                std::uint64_t seq, core::WallTime at, bool quiet) noexcept {
    // A stream's live chat lists nobody to tell.
    if (core::ports::is_stream_chat(room) || stopped_) {
        return;
    }
    const core::MonoTime now = clock_.now();
    try {
        Activity& a = activity_.try_emplace(room).first->second;
        a.used = now;
        if (!a.owed) {
            a.owed = ActivityNotice{
                .to = sender, .room = room, .seq = seq, .sender = sender, .at = at, .quiet = quiet};
        } else {
            ++counters_.activity_folded;
            // The newest names the notice; it is quiet only if everything it covers was.
            const bool all_quiet = a.owed->quiet && quiet;
            if (seq > a.owed->seq) {
                a.owed->seq = seq;
                a.owed->sender = sender;
                a.owed->at = at;
            }
            a.owed->quiet = all_quiet;
        }
        if (now >= a.next) {
            tell_activity(room, a);
            return;
        }
        if (!a.queued) {
            activity_due_.push(ActivityDue{.at = a.next, .room = room});
            a.queued = true;
        }
    } catch (const std::bad_alloc&) {
        // Nobody hears of this one; the next message, or a listing, says where the room is.
        ++counters_.allocation_failures;
    }
}

void ChatService::tell_activity(const core::RoomId& room, Activity& activity) noexcept {
    if (!activity.owed) {
        return;
    }
    const ActivityNotice notice = *activity.owed;
    activity.owed.reset();
    activity.next = clock_.now() + limits_.activity_interval;
    if (activity.members) {
        notify_members(*activity.members, notice);
        return;
    }
    if (stopped_) {
        return;
    }
    try {
        messages_.members(
            room, std::nullopt, core::ports::kMaxMembersPage,
            [this, room, generation = activity.generation,
             notice](core::ports::MessageResult<std::vector<core::UserId>> members) noexcept {
                activity_read(room, generation, notice, std::move(members));
            });
    } catch (const std::bad_alloc&) {
        ++counters_.allocation_failures;
        ++counters_.activity_failures;
    }
}

void ChatService::activity_read(
    const core::RoomId& room, std::uint64_t generation, const ActivityNotice& notice,
    core::ports::MessageResult<std::vector<core::UserId>> members) noexcept {
    if (!members) {
        ++counters_.activity_failures;
        return;
    }
    notify_members(*members, notice);
    // Kept for the room's next notices, unless its list changed since it was asked.
    const auto it = activity_.find(room);
    if (it != activity_.end() && it->second.generation == generation) {
        it->second.members = std::move(*members);
    }
}

void ChatService::notify_members(std::span<const core::UserId> members,
                                 ActivityNotice notice) noexcept {
    try {
        for (const core::UserId& member : members) {
            notice.to = member;
            const std::vector<std::byte> body = encode_activity(notice);
            rooms_plane_.notify(presence_room(member), body);
            ++counters_.activity_notices;
        }
    } catch (const std::bad_alloc&) {
        // The members after this one hear nothing of it.
        ++counters_.allocation_failures;
    }
}

void ChatService::sweep_activity(core::MonoTime now, bool full) noexcept {
    while (!activity_due_.empty() && activity_due_.top().at <= now) {
        const core::RoomId room = activity_due_.top().room;
        activity_due_.pop();
        const auto it = activity_.find(room);
        if (it == activity_.end()) {
            continue;
        }
        it->second.queued = false;
        tell_activity(room, it->second);
    }
    if (!full) {
        return;
    }
    // A room nobody here sent to for a linger: its list is read again if anyone does.
    std::erase_if(activity_, [&](const auto& entry) {
        const Activity& a = entry.second;
        return !a.owed && !a.queued && now - a.used >= limits_.linger;
    });
}

void ChatService::forget_members(const core::RoomId& room) noexcept {
    if (const auto it = activity_.find(room); it != activity_.end()) {
        it->second.members.reset();
        ++it->second.generation;
    }
}

} // namespace chat
