#include "ring.hpp"

#include "layout.hpp"
#include "presence_room.hpp"

#include <algorithm>
#include <chrono>
#include <new>

namespace chat {

namespace {

// The notice layout's first byte; a node that reads another drops the notice.
constexpr std::uint8_t kNoticeLayout = 1;

// A direct chat lists two members; the list is read up to a few more, and the call rings at
// most this many others, whatever a list that grew past two says.
constexpr std::size_t kMaxCallees = 7;

} // namespace

std::vector<std::byte> encode_notice(const CallNotice& notice) {
    std::vector<std::byte> out;
    layout::put_u8(out, kNoticeLayout);
    layout::put_u8(out, static_cast<std::uint8_t>(notice.event));
    layout::put_short(out, notice.to.view());
    layout::put_uuid(out, notice.room);
    layout::put_uuid(out, notice.call);
    layout::put_short(out, notice.from.view());
    layout::put_u8(out, notice.by ? 1 : 0);
    if (notice.by) {
        layout::put_short(out, notice.by->view());
    }
    const auto ms =
        std::chrono::duration_cast<core::Millis>(notice.expires_at.time_since_epoch()).count();
    layout::put_u64(out, static_cast<std::uint64_t>(ms));
    return out;
}

std::optional<CallNotice> decode_notice(std::span<const std::byte> bytes) {
    layout::Reader in{bytes};
    if (in.u8() != kNoticeLayout) {
        return std::nullopt;
    }
    const auto event = in.u8();
    if (!event || *event > static_cast<std::uint8_t>(RingEvent::Ended)) {
        return std::nullopt;
    }
    auto to = in.user();
    const auto room = in.uuid<core::RoomId>();
    const auto call = in.uuid<CallId>();
    auto from = in.user();
    const auto has_by = in.u8();
    if (!to || !room || !call || !from || !has_by || *has_by > 1) {
        return std::nullopt;
    }
    std::optional<core::UserId> by;
    if (*has_by == 1) {
        by = in.user();
        if (!by) {
            return std::nullopt;
        }
    }
    const auto ms = in.u64();
    if (!ms || !in.empty()) {
        return std::nullopt;
    }
    return CallNotice{.event = static_cast<RingEvent>(*event),
                      .to = *to,
                      .room = *room,
                      .call = *call,
                      .from = *from,
                      .by = by,
                      .expires_at =
                          core::WallTime{core::Millis{static_cast<core::Millis::rep>(*ms)}}};
}

Ringer::Ringer(IRingPlane& plane, const core::ports::IClock& clock, core::ports::IRandom& random,
               RingLimits limits)
    : plane_(plane), clock_(clock), random_(random), limits_(limits), next_prune_(clock.now()) {}

std::optional<core::Millis> Ringer::ring_limited(const core::RoomId& room,
                                                 const core::UserId& caller) noexcept {
    const auto it = histories_.find(room);
    if (it == histories_.end()) {
        return std::nullopt;
    }
    const History& h = it->second;
    const core::MonoTime now = clock_.now();
    core::Millis wait{0};
    if (h.declined == caller && now < h.declined_until) {
        wait = std::chrono::ceil<core::Millis>(h.declined_until - now);
    }
    // The oldest of the last rings_per_window starts frees a place when it leaves the window.
    if (limits_.rings_per_window > 0 && h.starts.size() >= limits_.rings_per_window) {
        const core::MonoTime frees = h.starts.front() + limits_.ring_window;
        if (now < frees) {
            wait = std::max(wait, std::chrono::ceil<core::Millis>(frees - now));
        }
    }
    if (wait <= core::Millis{0}) {
        return std::nullopt;
    }
    ++counters_.limited;
    return wait;
}

void Ringer::answering(const core::RoomId& room, const core::UserId& user) noexcept {
    const auto it = calls_.find(room);
    if (it == calls_.end()) {
        return;
    }
    Call& call = it->second;
    const core::MonoTime now = clock_.now();
    // Only an answer asked while the call still rings holds it; a late one cannot extend it.
    if (call.answered || now >= call.ring_deadline ||
        std::ranges::find(call.callees, user) == call.callees.end()) {
        return;
    }
    call.answering_until =
        std::max(call.answering_until, call.ring_deadline + limits_.answer_grace);
}

bool Ringer::remember(const core::RoomId& room) {
    if (histories_.contains(room) || histories_.size() < limits_.max_histories) {
        return true;
    }
    prune(clock_.now());
    return histories_.size() < limits_.max_histories;
}

void Ringer::prune(core::MonoTime now) noexcept {
    std::erase_if(histories_, [&](const auto& entry) {
        const History& h = entry.second;
        const bool rang_lately = !h.starts.empty() && now < h.starts.back() + limits_.ring_window;
        return !rang_lately && now >= h.declined_until && !calls_.contains(entry.first);
    });
}

bool Ringer::idle(const core::RoomId& room) const noexcept {
    return !calls_.contains(room);
}

bool Ringer::member_of(const Call& call, const core::UserId& user) noexcept {
    return call.caller == user || std::ranges::find(call.callees, user) != call.callees.end();
}

std::expected<std::optional<CallId>, RingRefusal>
Ringer::ticketed(const core::RoomId& room, const core::UserId& user,
                 const std::optional<std::vector<core::UserId>>& members) noexcept {
    try {
        const auto it = calls_.find(room);
        if (it != calls_.end()) {
            Call& call = it->second;
            if (!call.answered && call.caller != user &&
                std::ranges::find(call.callees, user) != call.callees.end()) {
                // The callee picked up: on every device of either, the ringing stops.
                call.answered = true;
                ++counters_.answered;
                announce(room, call, RingEvent::Answered, user);
            }
            if (call.answered) {
                call.hold_until = clock_.now() + limits_.answered_hold;
                schedule(room, call, call.hold_until);
            }
            return call.id;
        }
        // The ask found a call, which has ended since: this ticket was answering it.
        if (!members) {
            return std::nullopt;
        }
        std::vector<core::UserId> callees;
        for (const core::UserId& member : *members) {
            if (member != user && callees.size() < kMaxCallees &&
                std::ranges::find(callees, member) == callees.end()) {
                callees.push_back(member);
            }
        }
        if (callees.empty()) {
            return std::nullopt;
        }
        if (const auto wait = ring_limited(room, user)) {
            return std::unexpected(
                RingRefusal{.why = RingRefusal::Why::Limited, .retry_after = *wait});
        }
        if (full() || !remember(room)) {
            ++counters_.busy;
            return std::unexpected(RingRefusal{.why = RingRefusal::Why::Busy});
        }
        const CallId id = CallId::generate(clock_, random_);
        start(room, user, std::move(callees), id);
        return id;
    } catch (const std::bad_alloc&) {
        ++counters_.allocation_failures;
        return std::unexpected(RingRefusal{.why = RingRefusal::Why::Busy});
    }
}

void Ringer::start(const core::RoomId& room, const core::UserId& caller,
                   std::vector<core::UserId> callees, const CallId& id) {
    const core::MonoTime now = clock_.now();
    Call& call = calls_
                     .try_emplace(room, Call{.id = id,
                                             .caller = caller,
                                             .callees = std::move(callees),
                                             .answered = false,
                                             .ring_deadline = now + limits_.ring_timeout,
                                             .expires_at = clock_.wall_now() + limits_.ring_timeout,
                                             .next_announce = now + limits_.announce_every,
                                             .hold_until = now,
                                             .answering_until = now,
                                             .due = now})
                     .first->second;
    try {
        schedule(room, call, std::min(call.ring_deadline, call.next_announce));
        std::deque<core::MonoTime>& starts = histories_[room].starts;
        starts.push_back(now);
        while (starts.size() > std::max<std::size_t>(limits_.rings_per_window, 1)) {
            starts.pop_front();
        }
    } catch (const std::bad_alloc&) {
        // A call nothing would ever ring out is not kept.
        forget(calls_.find(room));
        throw;
    }
    ++counters_.started;
    announce(room, call, RingEvent::Ringing, std::nullopt);
}

void Ringer::schedule(const core::RoomId& room, Call& call, core::MonoTime due) {
    // The new entry first: if it cannot be made, the old one still stands.
    due_.emplace(due, room);
    if (call.due != due) {
        due_.erase({call.due, room});
    }
    call.due = due;
}

void Ringer::forget(Calls::iterator it) noexcept {
    due_.erase({it->second.due, it->first});
    calls_.erase(it);
}

std::optional<core::UserId> Ringer::signal(const core::RoomId& room, const core::UserId& user,
                                           CallSignal signal, const CallId& call) noexcept {
    const auto it = calls_.find(room);
    if (it == calls_.end() || it->second.id != call) {
        return std::nullopt;
    }
    Call& c = it->second;
    const bool callee = std::ranges::find(c.callees, user) != c.callees.end();
    RingEvent event = RingEvent::Ended;
    switch (signal) {
    case CallSignal::Decline:
        if (c.answered || !callee) {
            return std::nullopt;
        }
        event = RingEvent::Declined;
        ++counters_.declined;
        // remember() made the room's history when its ring started.
        if (const auto h = histories_.find(room); h != histories_.end()) {
            h->second.declined = c.caller;
            h->second.declined_until = clock_.now() + limits_.decline_cooldown;
        }
        break;
    case CallSignal::Cancel:
        if (c.answered || c.caller != user) {
            return std::nullopt;
        }
        event = RingEvent::Cancelled;
        ++counters_.cancelled;
        break;
    case CallSignal::End:
        if (!c.answered || !member_of(c, user)) {
            return std::nullopt;
        }
        event = RingEvent::Ended;
        ++counters_.ended;
        break;
    }
    const core::UserId caller = c.caller;
    announce(room, c, event, user);
    forget(it);
    return caller;
}

void Ringer::tick() noexcept {
    const core::MonoTime now = clock_.now();
    // The histories are looked at once a second: the limits are seconds long.
    if (now >= next_prune_) {
        next_prune_ = now + core::Millis{1'000};
        prune(now);
    }
    while (!due_.empty() && due_.begin()->first <= now) {
        const core::RoomId room = due_.begin()->second;
        const auto it = calls_.find(room);
        if (it == calls_.end()) {
            due_.erase(due_.begin());
            continue;
        }
        Call& call = it->second;
        // A deposed owner says nothing: the new one knows nothing of the call, and what this
        // one said could contradict it. The devices stop ringing at expires_at by themselves.
        if (!plane_.owns(room)) {
            ++counters_.orphaned;
            forget(it);
            continue;
        }
        if (call.answered) {
            forget(it);
            continue;
        }
        if (now >= call.ring_deadline && now < call.answering_until) {
            // A callee's ticket is on its way: rung out only if it never comes.
            ++counters_.graced;
            try {
                schedule(room, call, call.answering_until);
                continue;
            } catch (const std::bad_alloc&) {
                ++counters_.allocation_failures;
            }
        }
        if (now >= call.ring_deadline) {
            ++counters_.missed;
            announce(room, call, RingEvent::Missed, std::nullopt);
            forget(it);
            continue;
        }
        announce(room, call, RingEvent::Ringing, std::nullopt);
        while (call.next_announce <= now) {
            call.next_announce += limits_.announce_every;
        }
        try {
            schedule(room, call, std::min(call.ring_deadline, call.next_announce));
        } catch (const std::bad_alloc&) {
            // Rung out now rather than kept with nothing to ring it out later.
            ++counters_.allocation_failures;
            ++counters_.missed;
            announce(room, call, RingEvent::Missed, std::nullopt);
            forget(it);
        }
    }
}

void Ringer::announce(const core::RoomId& room, const Call& call, RingEvent event,
                      const std::optional<core::UserId>& by) noexcept {
    const auto tell = [&](const core::UserId& to) noexcept {
        try {
            const std::vector<std::byte> notice = encode_notice({.event = event,
                                                                 .to = to,
                                                                 .room = room,
                                                                 .call = call.id,
                                                                 .from = call.caller,
                                                                 .by = by,
                                                                 .expires_at = call.expires_at});
            ++counters_.notices;
            plane_.notify(presence_room(to), notice);
        } catch (const std::bad_alloc&) {
            ++counters_.allocation_failures;
        }
    };
    tell(call.caller);
    for (const core::UserId& callee : call.callees) {
        tell(callee);
    }
}

} // namespace chat
