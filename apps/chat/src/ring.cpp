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
    // After everything a node of the first ring read, so its notices read as they did there.
    if (notice.event == RingEvent::Moved) {
        layout::put_short(out, notice.subject.value_or(notice.to).view());
    }
    return out;
}

std::optional<CallNotice> decode_notice(std::span<const std::byte> bytes) {
    layout::Reader in{bytes};
    if (in.u8() != kNoticeLayout) {
        return std::nullopt;
    }
    const auto event = in.u8();
    if (!event || *event > static_cast<std::uint8_t>(RingEvent::Moved)) {
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
    if (!ms) {
        return std::nullopt;
    }
    std::optional<core::UserId> subject;
    if (static_cast<RingEvent>(*event) == RingEvent::Moved) {
        subject = in.user();
        if (!subject) {
            return std::nullopt;
        }
    }
    if (!in.empty()) {
        return std::nullopt;
    }
    return CallNotice{.event = static_cast<RingEvent>(*event),
                      .to = *to,
                      .room = *room,
                      .call = *call,
                      .from = *from,
                      .by = by,
                      .expires_at =
                          core::WallTime{core::Millis{static_cast<core::Millis::rep>(*ms)}},
                      .subject = subject};
}

Ringer::Ringer(IRingPlane& plane, const core::ports::IClock& clock, core::ports::IRandom& random,
               RingLimits limits)
    : plane_(plane), clock_(clock), random_(random), limits_(limits),
      window_rings_(std::clamp<std::size_t>(limits.rings_per_window, 1, kMaxRingsPerWindow)),
      next_prune_(clock.now()) {}

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
    if (h.count >= window_rings_) {
        // Full, the next to be written over is the oldest.
        const core::MonoTime frees = h.starts.at(h.next) + limits_.ring_window;
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
    // At the cap every ring attempt would walk all the histories: once a second is enough, as
    // the limits are seconds long.
    const core::MonoTime now = clock_.now();
    if (now >= next_prune_) {
        next_prune_ = now + core::Millis{1'000};
        prune(now);
    }
    return histories_.size() < limits_.max_histories;
}

void Ringer::prune(core::MonoTime now) noexcept {
    std::erase_if(histories_, [&](const auto& entry) {
        const History& h = entry.second;
        const std::size_t newest = (h.next + window_rings_ - 1) % window_rings_;
        const bool rang_lately = h.count > 0 && now < h.starts.at(newest) + limits_.ring_window;
        return !rang_lately && now >= h.declined_until && !calls_.contains(entry.first);
    });
}

bool Ringer::idle(const core::RoomId& room) const noexcept {
    return !calls_.contains(room);
}

bool Ringer::member_of(const Call& call, const core::UserId& user) noexcept {
    return call.caller == user || std::ranges::find(call.callees, user) != call.callees.end() ||
           std::ranges::find(call.joined, user) != call.joined.end();
}

bool Ringer::expelled(const core::RoomId& room, const core::UserId& user) const noexcept {
    const auto it = calls_.find(room);
    return it != calls_.end() &&
           std::ranges::find(it->second.expelled, user) != it->second.expelled.end();
}

bool Ringer::fits(const core::RoomId& room, const core::UserId& user) const noexcept {
    const auto it = calls_.find(room);
    if (it == calls_.end() || it->second.kind != CallKind::Group) {
        return true;
    }
    const Call& call = it->second;
    return call.joined.size() < kMaxGroupJoined ||
           std::ranges::find(call.joined, user) != call.joined.end();
}

std::optional<CallId> Ringer::call_of(const core::RoomId& room) const noexcept {
    const auto it = calls_.find(room);
    if (it == calls_.end()) {
        return std::nullopt;
    }
    return it->second.id;
}

bool Ringer::answerable(const core::RoomId& room, const core::UserId& user) const noexcept {
    const auto it = calls_.find(room);
    if (it == calls_.end()) {
        return false;
    }
    const Call& call = it->second;
    // A direct call still ringing from the asker is theirs, not one they could answer: the call
    // they were answering crossed with a new one of their own.
    return call.kind == CallKind::Group || call.answered || call.caller != user;
}

std::optional<CallKind> Ringer::kind_of(const core::RoomId& room) const noexcept {
    const auto it = calls_.find(room);
    if (it == calls_.end()) {
        return std::nullopt;
    }
    return it->second.kind;
}

std::vector<core::UserId> Ringer::joined(const core::RoomId& room) const {
    const auto it = calls_.find(room);
    if (it == calls_.end()) {
        return {};
    }
    const Call& call = it->second;
    if (call.kind == CallKind::Group) {
        return call.joined;
    }
    // A direct call's tickets: the caller's, and the callee's once answered.
    std::vector<core::UserId> out{call.caller};
    if (call.answered) {
        out.insert(out.end(), call.callees.begin(), call.callees.end());
    }
    return out;
}

std::vector<core::RoomId> Ringer::rooms() const {
    std::vector<core::RoomId> out;
    out.reserve(calls_.size());
    for (const auto& [room, call] : calls_) {
        out.push_back(room);
    }
    return out;
}

std::expected<std::optional<CallId>, RingRefusal>
Ringer::ticketed(const core::RoomId& room, const core::UserId& user,
                 const std::optional<std::vector<core::UserId>>& members, CallKind kind) noexcept {
    try {
        const auto it = calls_.find(room);
        if (it != calls_.end()) {
            Call& call = it->second;
            if (std::ranges::find(call.expelled, user) != call.expelled.end()) {
                return std::unexpected(RingRefusal{.why = RingRefusal::Why::Expelled});
            }
            if (call.kind == CallKind::Group) {
                if (!fits(room, user)) {
                    ++counters_.busy;
                    return std::unexpected(RingRefusal{.why = RingRefusal::Why::Busy});
                }
                joined_group(room, call, user);
                return call.id;
            }
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
        const std::size_t most = kind == CallKind::Group ? kMaxGroupCallees : kMaxCallees;
        std::vector<core::UserId> callees;
        for (const core::UserId& member : *members) {
            if (member != user && callees.size() < most &&
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
        start(room, user, std::move(callees), id, kind);
        return id;
    } catch (const std::bad_alloc&) {
        ++counters_.allocation_failures;
        return std::unexpected(RingRefusal{.why = RingRefusal::Why::Busy});
    }
}

void Ringer::joined_group(const core::RoomId& room, Call& call, const core::UserId& user) {
    const bool was_ringing = std::ranges::find(call.ringing, user) != call.ringing.end();
    const bool was_in = std::ranges::find(call.joined, user) != call.joined.end();
    if (!was_in) {
        call.joined.push_back(user);
    }
    std::erase(call.ringing, user);
    if (user != call.caller && !call.answered) {
        call.answered = true;
        ++counters_.answered;
    }
    // Joining is answering, for whoever was not in the call: everyone hears who came, and the
    // member's other devices stop ringing.
    if (was_ringing || (!was_in && call.answered)) {
        announce(room, call, RingEvent::Answered, user);
    }
    if (call.answered) {
        call.hold_until = clock_.now() + limits_.answered_hold;
    }
    reschedule(room, call);
}

void Ringer::start(const core::RoomId& room, const core::UserId& caller,
                   std::vector<core::UserId> callees, const CallId& id, CallKind kind) {
    const core::MonoTime now = clock_.now();
    std::vector<core::UserId> ringing;
    std::vector<core::UserId> joined;
    if (kind == CallKind::Group) {
        ringing = callees;
        joined.push_back(caller);
    }
    Call& call = calls_
                     .try_emplace(room, Call{.id = id,
                                             .caller = caller,
                                             .callees = std::move(callees),
                                             .answered = false,
                                             .kind = kind,
                                             .ringing = std::move(ringing),
                                             .joined = std::move(joined),
                                             .ring_deadline = now + limits_.ring_timeout,
                                             .expires_at = clock_.wall_now() + limits_.ring_timeout,
                                             .next_announce = now + limits_.announce_every,
                                             .hold_until = now,
                                             .answering_until = now,
                                             .due = now})
                     .first->second;
    try {
        schedule(room, call, std::min(call.ring_deadline, call.next_announce));
        History& h = histories_[room];
        h.starts.at(h.next) = now;
        h.next = static_cast<std::uint8_t>((h.next + 1U) % window_rings_);
        h.count = static_cast<std::uint8_t>(std::min<std::size_t>(h.count + 1U, window_rings_));
        // The member who declined calling back: the caller they declined is no longer held.
        if (h.declined && *h.declined != caller) {
            h.declined.reset();
            h.declined_until = {};
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

void Ringer::reschedule(const core::RoomId& room, Call& call) {
    std::optional<core::MonoTime> due;
    const auto sooner = [&](core::MonoTime t) { due = due ? std::min(*due, t) : t; };
    const bool rings = !call.answered || !call.ringing.empty();
    if (rings) {
        sooner(std::max(call.ring_deadline, call.answering_until));
        if (clock_.now() < call.ring_deadline) {
            sooner(std::min(call.ring_deadline, call.next_announce));
        }
    }
    if (call.answered && !call.checking) {
        sooner(call.hold_until);
    }
    if (!due) {
        // Waiting on the SFU's answer, which reschedules it.
        due_.erase({call.due, room});
        call.due = core::MonoTime::max();
        return;
    }
    schedule(room, call, *due);
}

void Ringer::forget(Calls::iterator it) noexcept {
    due_.erase({it->second.due, it->first});
    calls_.erase(it);
}

void Ringer::forget_ended(Calls::iterator it) noexcept {
    const bool group = it->second.kind == CallKind::Group;
    const core::RoomId room = it->first;
    forget(it);
    if (group && group_ended_) {
        group_ended_(room);
    }
}

std::optional<core::UserId> Ringer::signal(const core::RoomId& room, const core::UserId& user,
                                           CallSignal signal, const CallId& call) noexcept {
    const auto it = calls_.find(room);
    if (it == calls_.end() || it->second.id != call) {
        return std::nullopt;
    }
    Call& c = it->second;
    const bool group = c.kind == CallKind::Group;
    const bool callee = std::ranges::find(c.callees, user) != c.callees.end();
    RingEvent event = RingEvent::Ended;
    switch (signal) {
    case CallSignal::Decline:
        if (group) {
            if (std::ranges::find(c.ringing, user) == c.ringing.end()) {
                return std::nullopt;
            }
            std::erase(c.ringing, user);
            ++counters_.declined;
            const core::UserId caller = c.caller;
            announce(room, c, RingEvent::Declined, user);
            // Everyone rung turned it down before anyone came: nobody answered.
            if (!c.answered && c.ringing.empty()) {
                ++counters_.missed;
                announce(room, c, RingEvent::Missed, std::nullopt);
                forget_ended(it);
            }
            return caller;
        }
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
        // A group call is ended for everyone by its caller, through may_end().
        if (group || !c.answered || !member_of(c, user)) {
            return std::nullopt;
        }
        event = RingEvent::Ended;
        ++counters_.ended;
        break;
    case CallSignal::Leave: {
        if (!group || std::ranges::find(c.joined, user) == c.joined.end()) {
            return std::nullopt;
        }
        std::erase(c.joined, user);
        ++counters_.left;
        const core::UserId caller = c.caller;
        announce(room, c, RingEvent::Left, user);
        if (c.joined.empty() && !c.checking) {
            // The last one known: the SFU says whether anyone is still in it (a device that
            // joined from a ticket of an earlier owner, a member who left without saying so
            // left nobody behind), and nobody there ends it.
            try {
                checks_.emplace_back(room, c.id);
                c.checking = true;
            } catch (const std::bad_alloc&) {
                // Asked at the next hold's end instead.
                ++counters_.allocation_failures;
            }
        }
        return caller;
    }
    case CallSignal::Expel:
        // Through expel(), which the call handler follows with its fenced write.
        return std::nullopt;
    }
    const core::UserId caller = c.caller;
    announce(room, c, event, user);
    forget(it);
    return caller;
}

std::optional<MediaStepNeeded> Ringer::may_end(const core::RoomId& room, const core::UserId& user,
                                               const CallId& call) const noexcept {
    const auto it = calls_.find(room);
    if (it == calls_.end() || it->second.id != call || it->second.kind != CallKind::Group ||
        it->second.caller != user) {
        return std::nullopt;
    }
    return MediaStepNeeded::Close;
}

std::optional<MediaStepNeeded> Ringer::expel(const core::RoomId& room, const core::UserId& user,
                                             const CallId& call,
                                             const core::UserId& target) noexcept {
    const auto it = calls_.find(room);
    if (it == calls_.end() || it->second.id != call || it->second.kind != CallKind::Group ||
        it->second.caller != user || target == user) {
        return std::nullopt;
    }
    try {
        return put_out(room, it->second, target);
    } catch (const std::bad_alloc&) {
        ++counters_.allocation_failures;
        return std::nullopt;
    }
}

MediaStepNeeded Ringer::put_out(const core::RoomId& room, Call& call, const core::UserId& target) {
    if (std::ranges::find(call.expelled, target) == call.expelled.end()) {
        // The oldest is forgotten first: it was put out of the media room long ago.
        if (call.expelled.size() >= kMaxExpelled) {
            call.expelled.erase(call.expelled.begin());
        }
        call.expelled.push_back(target);
    }
    ++counters_.expelled;
    std::erase(call.ringing, target);
    std::erase(call.callees, target);
    const bool had_ticket = std::ranges::find(call.joined, target) != call.joined.end();
    std::erase(call.joined, target);
    reschedule(room, call);
    return had_ticket ? MediaStepNeeded::Move : MediaStepNeeded::None;
}

MediaStepNeeded Ringer::removed(const core::RoomId& room, const core::UserId& user) noexcept {
    const auto it = calls_.find(room);
    if (it == calls_.end() || !member_of(it->second, user)) {
        return MediaStepNeeded::None;
    }
    Call& call = it->second;
    if (call.kind == CallKind::Direct) {
        // A direct chat without one of its two members has no call left: it ends, and its media
        // room closes under whoever is still in it.
        return MediaStepNeeded::Close;
    }
    try {
        const MediaStepNeeded step = put_out(room, call, user);
        if (!call.answered && call.ringing.empty() && call.joined.size() <= 1 &&
            step == MediaStepNeeded::None) {
            // Nobody left to ring: the caller's call was missed.
            ++counters_.missed;
            announce(room, call, RingEvent::Missed, std::nullopt);
            forget_ended(it);
        }
        return step;
    } catch (const std::bad_alloc&) {
        ++counters_.allocation_failures;
        return MediaStepNeeded::Move;
    }
}

void Ringer::ended(const core::RoomId& room, const CallId& call,
                   const std::optional<core::UserId>& by) noexcept {
    const auto it = calls_.find(room);
    if (it == calls_.end() || it->second.id != call) {
        return;
    }
    ++counters_.ended;
    announce(room, it->second, RingEvent::Ended, by);
    forget(it);
}

void Ringer::moved(const core::RoomId& room, const CallId& call,
                   const std::optional<core::UserId>& by, const core::UserId& subject,
                   bool everyone) noexcept {
    const auto it = calls_.find(room);
    if (it == calls_.end() || it->second.id != call) {
        return;
    }
    const Call& c = it->second;
    if (!everyone) {
        // Nothing moved: only the one put out needs to know.
        tell(room, c, subject, RingEvent::Moved, by, subject);
        return;
    }
    try {
        for (const core::UserId& to : members(c)) {
            tell(room, c, to, RingEvent::Moved, by, subject);
        }
    } catch (const std::bad_alloc&) {
        ++counters_.allocation_failures;
    }
    if (!member_of(c, subject)) {
        tell(room, c, subject, RingEvent::Moved, by, subject);
    }
}

std::vector<std::pair<core::RoomId, CallId>> Ringer::take_checks() {
    return std::exchange(checks_, {});
}

void Ringer::occupied(const core::RoomId& room, const CallId& call, std::optional<bool> anyone,
                      bool asked) noexcept {
    const auto it = calls_.find(room);
    if (it == calls_.end() || it->second.id != call || !it->second.checking) {
        return;
    }
    Call& c = it->second;
    c.checking = false;
    // An SFU that cannot say, so many checks running, is taken to hold nobody: a room it has
    // dropped answers no better, and the call would otherwise never end.
    if (anyone) {
        c.unanswered = 0;
    } else if (asked) {
        ++c.unanswered;
    }
    if (anyone == false || c.unanswered >= kMaxUnansweredChecks) {
        ++counters_.emptied;
        announce(room, c, RingEvent::Ended, std::nullopt);
        forget_ended(it);
        return;
    }
    c.hold_until = clock_.now() + limits_.occupancy_check;
    try {
        reschedule(room, c);
    } catch (const std::bad_alloc&) {
        // Kept with nothing due would be kept for ever: forgotten without a word instead.
        ++counters_.allocation_failures;
        forget_ended(it);
    }
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
        if (call.kind == CallKind::Group) {
            tick_group(room, it, now);
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

void Ringer::tick_group(const core::RoomId& room, Calls::iterator it, core::MonoTime now) noexcept {
    Call& call = it->second;
    const bool rings = !call.answered || !call.ringing.empty();
    if (rings && now >= call.ring_deadline) {
        if (now < call.answering_until) {
            // A callee's ticket is on its way: rung out only if it never comes.
            if (call.due < call.answering_until) {
                ++counters_.graced;
            }
        } else if (!call.answered) {
            ++counters_.missed;
            announce(room, call, RingEvent::Missed, std::nullopt);
            forget_ended(it);
            return;
        } else {
            // The call goes on; each member still rung missed it, and only they are told.
            for (const core::UserId& member : call.ringing) {
                ++counters_.missed;
                tell(room, call, member, RingEvent::Missed, std::nullopt);
            }
            call.ringing.clear();
        }
    } else if (rings && now >= call.next_announce) {
        // Again, to those still rung: the caller until someone comes, and the callees.
        if (!call.answered) {
            tell(room, call, call.caller, RingEvent::Ringing, std::nullopt);
        }
        for (const core::UserId& member : call.ringing) {
            tell(room, call, member, RingEvent::Ringing, std::nullopt);
        }
        while (call.next_announce <= now) {
            call.next_announce += limits_.announce_every;
        }
    }
    if (call.answered && !call.checking && now >= call.hold_until) {
        try {
            checks_.emplace_back(room, call.id);
            call.checking = true;
        } catch (const std::bad_alloc&) {
            // Asked at the next tick.
            ++counters_.allocation_failures;
        }
    }
    try {
        reschedule(room, call);
    } catch (const std::bad_alloc&) {
        // Kept with nothing due would be kept for ever: forgotten without a word instead.
        ++counters_.allocation_failures;
        forget_ended(it);
    }
}

std::vector<core::UserId> Ringer::members(const Call& call) {
    std::vector<core::UserId> out{call.caller};
    for (const core::UserId& callee : call.callees) {
        out.push_back(callee);
    }
    for (const core::UserId& in : call.joined) {
        if (std::ranges::find(out, in) == out.end()) {
            out.push_back(in);
        }
    }
    return out;
}

void Ringer::tell(const core::RoomId& room, const Call& call, const core::UserId& to,
                  RingEvent event, const std::optional<core::UserId>& by,
                  const std::optional<core::UserId>& subject) noexcept {
    try {
        const std::vector<std::byte> notice = encode_notice({.event = event,
                                                             .to = to,
                                                             .room = room,
                                                             .call = call.id,
                                                             .from = call.caller,
                                                             .by = by,
                                                             .expires_at = call.expires_at,
                                                             .subject = subject});
        ++counters_.notices;
        plane_.notify(presence_room(to), notice);
    } catch (const std::bad_alloc&) {
        ++counters_.allocation_failures;
    }
}

void Ringer::announce(const core::RoomId& room, const Call& call, RingEvent event,
                      const std::optional<core::UserId>& by) noexcept {
    if (call.kind == CallKind::Direct) {
        tell(room, call, call.caller, event, by);
        for (const core::UserId& callee : call.callees) {
            tell(room, call, callee, event, by);
        }
        return;
    }
    try {
        for (const core::UserId& to : members(call)) {
            tell(room, call, to, event, by);
        }
    } catch (const std::bad_alloc&) {
        ++counters_.allocation_failures;
    }
}

} // namespace chat
