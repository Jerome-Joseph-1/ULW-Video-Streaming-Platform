#include "call.hpp"

#include "layout.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <string_view>
#include <utility>

namespace chat {

namespace {

// The layouts' first byte; a node that reads another is answered Unavailable, as for an owner
// that cannot be reached. 2: an ask says what it asks for, and a ticket names its call.
constexpr std::uint8_t kLayout = 2;
// What an ask asks for: a ticket, or one of CallSignal's values.
constexpr std::uint8_t kTicket = 0;

// A direct chat lists two members; a few more are read, for a list that grew. A group call
// rings at most kMaxGroupCallees others, the caller aside.
constexpr std::size_t kMembersRead = 8;
constexpr std::size_t kGroupMembersRead = kMaxGroupCallees + 1;
// A move asked while this many wait in the room is answered busy: the caller's own, in order.
constexpr std::size_t kMaxMovesWaiting = 16;
// A removal's move the store could not take, and an old media room the SFU could not close,
// are tried again after this.
constexpr core::Millis kRetryEvery{1'000};

} // namespace

using layout::put_long;
using layout::put_short;
using layout::put_u64;
using layout::put_u8;
using layout::put_uuid;

std::vector<std::byte> encode_request(const CallRequest& request) {
    std::vector<std::byte> out;
    put_u8(out, kLayout);
    put_u8(out, kTicket);
    put_short(out, request.user.view());
    put_uuid(out, request.device);
    return out;
}

std::vector<std::byte> encode_request(const CallSignalRequest& request) {
    std::vector<std::byte> out;
    put_u8(out, kLayout);
    put_u8(out, static_cast<std::uint8_t>(request.signal));
    put_short(out, request.user.view());
    put_uuid(out, request.call);
    if (request.signal == CallSignal::Expel) {
        put_short(out, request.target.value_or(request.user).view());
    }
    return out;
}

std::optional<CallAsk> decode_request(std::span<const std::byte> bytes) {
    layout::Reader in{bytes};
    if (in.u8() != kLayout) {
        return std::nullopt;
    }
    const auto what = in.u8();
    auto user = in.user();
    if (!what || !user || *what > static_cast<std::uint8_t>(CallSignal::Expel)) {
        return std::nullopt;
    }
    if (*what == kTicket) {
        const auto device = in.uuid<core::DeviceId>();
        if (!device || !in.empty()) {
            return std::nullopt;
        }
        return CallRequest{.user = *user, .device = *device};
    }
    const auto call = in.uuid<CallId>();
    if (!call) {
        return std::nullopt;
    }
    const auto signal = static_cast<CallSignal>(*what);
    std::optional<core::UserId> target;
    if (signal == CallSignal::Expel) {
        target = in.user();
        if (!target) {
            return std::nullopt;
        }
    }
    if (!in.empty()) {
        return std::nullopt;
    }
    return CallSignalRequest{.user = *user, .signal = signal, .call = *call, .target = target};
}

std::vector<std::byte> encode_answer(const CallAnswer& answer) {
    std::vector<std::byte> out;
    put_u8(out, kLayout);
    put_u8(out, static_cast<std::uint8_t>(answer.outcome));
    if (answer.outcome == CallOutcome::Ticket && answer.ticket) {
        put_long(out, answer.ticket->endpoint);
        put_long(out, answer.ticket->credential);
        const auto ms =
            std::chrono::duration_cast<core::Millis>(answer.ticket->expires_at.time_since_epoch())
                .count();
        put_u64(out, static_cast<std::uint64_t>(ms));
        put_u8(out, answer.call ? 1 : 0);
        if (answer.call) {
            put_uuid(out, *answer.call);
        }
    }
    if (answer.outcome == CallOutcome::Done && answer.caller) {
        put_short(out, answer.caller->view());
    }
    if (answer.outcome == CallOutcome::RingLimited) {
        put_u64(out,
                static_cast<std::uint64_t>(answer.retry_after.value_or(core::Millis{0}).count()));
    }
    return out;
}

std::optional<CallAnswer> decode_answer(std::span<const std::byte> bytes) {
    layout::Reader in{bytes};
    if (in.u8() != kLayout) {
        return std::nullopt;
    }
    const auto outcome = in.u8();
    if (!outcome || *outcome > static_cast<std::uint8_t>(CallOutcome::Full)) {
        return std::nullopt;
    }
    CallAnswer answer{.outcome = static_cast<CallOutcome>(*outcome), .ticket = std::nullopt};
    if (answer.outcome == CallOutcome::Ticket) {
        const auto endpoint = in.long_text();
        const auto credential = in.long_text();
        const auto ms = in.u64();
        const auto has_call = in.u8();
        if (!endpoint || !credential || !ms || !has_call || *has_call > 1) {
            return std::nullopt;
        }
        answer.ticket = core::ports::MediaTicket{
            .endpoint = std::string(*endpoint),
            .credential = std::string(*credential),
            .expires_at = core::WallTime{core::Millis{static_cast<core::Millis::rep>(*ms)}}};
        if (*has_call == 1) {
            answer.call = in.uuid<CallId>();
            if (!answer.call) {
                return std::nullopt;
            }
        }
    }
    if (answer.outcome == CallOutcome::Done) {
        answer.caller = in.user();
        if (!answer.caller) {
            return std::nullopt;
        }
    }
    if (answer.outcome == CallOutcome::RingLimited) {
        const auto ms = in.u64();
        if (!ms ||
            *ms > static_cast<std::uint64_t>(std::numeric_limits<core::Millis::rep>::max())) {
            return std::nullopt;
        }
        answer.retry_after = core::Millis{static_cast<core::Millis::rep>(*ms)};
    }
    if (!in.empty()) {
        return std::nullopt;
    }
    return answer;
}

CallHandler::CallHandler(core::ports::IMessageStore& messages, core::ports::ISfu* sfu,
                         ICallPlane& plane, const core::ports::IClock& clock,
                         core::ports::IRandom& random, CallLimits limits)
    : messages_(messages), sfu_(sfu), plane_(plane), clock_(clock), limits_(limits),
      ringer_(plane, clock, random, limits.ring), next_sweep_(clock.now()) {
    limits_.group_participants =
        std::clamp(limits_.group_participants, kMinGroupParticipants, kMaxGroupParticipants);
}

CallHandler::~CallHandler() = default;

void CallHandler::on_ask(const core::RoomId& room, std::span<const std::byte> request,
                         rt::OwnerAnswer answer) noexcept {
    bool counted = false;
    try {
        const auto asked = decode_request(request);
        if (!asked) {
            answer(std::unexpected(rt::RouteError::Unavailable));
            return;
        }
        if (sfu_ == nullptr) {
            finish(answer, {.outcome = CallOutcome::Disabled, .ticket = std::nullopt});
            return;
        }
        if (in_flight_ >= limits_.max_in_flight) {
            ++counters_.busy;
            finish(answer, {.outcome = CallOutcome::Busy, .ticket = std::nullopt});
            return;
        }
        ++in_flight_;
        counted = true;
        if (const auto* s = std::get_if<CallSignalRequest>(&*asked)) {
            signal(room, *s, std::move(answer));
            return;
        }
        // The member list, read on the owner at the moment of asking: a client's join may be
        // older than a removal still on its way to its node (ADR-0073).
        messages_.access(
            room, std::get<CallRequest>(*asked).user,
            [this, room,
             waiter =
                 Waiter{.request = std::get<CallRequest>(*asked), .answer = std::move(answer)}](
                core::ports::MessageResult<core::ports::RoomAccess> access) mutable noexcept {
                checked(room, std::move(waiter), access);
            });
    } catch (const std::bad_alloc&) {
        if (counted) {
            --in_flight_;
        }
        // Whatever was moved out of `answer` answers nothing; the asker times out.
        if (answer) {
            answer(std::unexpected(rt::RouteError::Unavailable));
        }
    }
}

void CallHandler::signal(const core::RoomId& room, const CallSignalRequest& request,
                         rt::OwnerAnswer answer) {
    // The member list again, as for a ticket: only a member may move the call.
    messages_.access(
        room, request.user,
        [this, room, request, answer = std::move(answer)](
            core::ports::MessageResult<core::ports::RoomAccess> access) mutable noexcept {
            --in_flight_;
            signalled(room, request, answer, access);
        });
}

void CallHandler::signalled(
    const core::RoomId& room, const CallSignalRequest& request, rt::OwnerAnswer& answer,
    const core::ports::MessageResult<core::ports::RoomAccess>& access) noexcept {
    if (!callable(access, answer)) {
        return;
    }
    const bool group = access->kind == core::ports::RoomKind::GroupChat;
    if (request.signal == CallSignal::Expel || (group && request.signal == CallSignal::End)) {
        moderate(room, request, answer);
        return;
    }
    const auto caller = ringer_.signal(room, request.user, request.signal, request.call);
    if (!caller) {
        ++counters_.no_call;
        finish(answer, {.outcome = CallOutcome::NoCall, .ticket = std::nullopt});
        return;
    }
    finish(answer, {.outcome = CallOutcome::Done,
                    .ticket = std::nullopt,
                    .call = request.call,
                    .caller = caller});
}

void CallHandler::moderate(const core::RoomId& room, const CallSignalRequest& request,
                           rt::OwnerAnswer& answer) noexcept {
    const bool end = request.signal == CallSignal::End;
    const auto step = end ? ringer_.may_end(room, request.user, request.call)
                      : request.target
                          ? ringer_.expel(room, request.user, request.call, *request.target)
                          : std::nullopt;
    if (!step) {
        ++counters_.no_call;
        finish(answer, {.outcome = CallOutcome::NoCall, .ticket = std::nullopt});
        return;
    }
    if (retired_.size() >= limits_.max_retired) {
        ++counters_.busy;
        finish(answer, {.outcome = CallOutcome::Busy, .ticket = std::nullopt});
        return;
    }
    const auto it = rooms_.find(room);
    if (it != rooms_.end() && it->second.moves.size() >= kMaxMovesWaiting) {
        ++counters_.busy;
        finish(answer, {.outcome = CallOutcome::Busy, .ticket = std::nullopt});
        return;
    }
    Move move{.step = *step,
              .call = request.call,
              .by = request.user,
              .subject = request.target,
              .answer = std::move(answer)};
    if (*step == MediaStepNeeded::None) {
        // No ticket in this call; but a device may still be connected from before this node
        // owned the room, which only the SFU knows.
        move_if_connected(room, CallKind::Group, std::move(move));
        return;
    }
    enqueue(room, CallKind::Group, std::move(move));
}

void CallHandler::move_if_connected(const core::RoomId& room, CallKind kind, Move move) noexcept {
    const auto it = rooms_.find(room);
    if (it == rooms_.end() || !it->second.media || it->second.busy) {
        // Nothing here to ask with: moved all the same, which costs everyone a reconnect.
        move.step = MediaStepNeeded::Move;
        enqueue(room, kind, std::move(move));
        return;
    }
    try {
        auto slot = std::make_shared<Move>(std::move(move));
        it->second.media->participants(
            [this, room, kind, slot](
                std::expected<std::vector<core::ports::MediaParticipant>, core::ports::MediaError>
                    listed) noexcept {
                Move& m = *slot;
                const bool connected = !listed || std::ranges::any_of(*listed, [&](const auto& p) {
                    return m.subject && p.user == *m.subject;
                });
                if (connected) {
                    m.step = MediaStepNeeded::Move;
                    enqueue(room, kind, std::move(m));
                    return;
                }
                // Put out before it had a ticket or a connection: its tickets are refused from
                // now on, and there is no media room to keep it out of.
                if (m.call && m.subject) {
                    ringer_.moved(room, *m.call, m.by, *m.subject);
                }
                if (m.answer) {
                    finish(m.answer, {.outcome = CallOutcome::Done,
                                      .ticket = std::nullopt,
                                      .call = m.call,
                                      .caller = m.by});
                }
            });
    } catch (const std::bad_alloc&) {
        // The move, if any part of it is left, answers nothing; the asker times out.
    }
}

bool CallHandler::callable(const core::ports::MessageResult<core::ports::RoomAccess>& access,
                           rt::OwnerAnswer& answer) noexcept {
    if (!access) {
        ++counters_.store_unavailable;
        finish(answer, {.outcome = CallOutcome::Unavailable, .ticket = std::nullopt});
        return false;
    }
    if (access->kind != core::ports::RoomKind::DirectChat &&
        access->kind != core::ports::RoomKind::GroupChat) {
        ++counters_.not_callable;
        finish(answer, {.outcome = CallOutcome::NotCallable, .ticket = std::nullopt});
        return false;
    }
    if (!access->member) {
        ++counters_.not_member;
        finish(answer, {.outcome = CallOutcome::NotMember, .ticket = std::nullopt});
        return false;
    }
    return true;
}

void CallHandler::checked(const core::RoomId& room, Waiter waiter,
                          core::ports::MessageResult<core::ports::RoomAccess> access) noexcept {
    if (!callable(access, waiter.answer)) {
        --in_flight_;
        return;
    }
    waiter.kind =
        access->kind == core::ports::RoomKind::GroupChat ? CallKind::Group : CallKind::Direct;
    if (!ringer_.idle(room)) {
        if (ringer_.expelled(room, waiter.request.user)) {
            ++counters_.expelled;
            --in_flight_;
            finish(waiter.answer, {.outcome = CallOutcome::Expelled, .ticket = std::nullopt});
            return;
        }
        if (!ringer_.fits(room, waiter.request.user)) {
            ++counters_.busy;
            --in_flight_;
            finish(waiter.answer, {.outcome = CallOutcome::Busy, .ticket = std::nullopt});
            return;
        }
        // A callee answering: the ring waits for this ticket a little past its timeout.
        ringer_.answering(room, waiter.request.user);
        admit(room, std::move(waiter));
        return;
    }
    // This ticket may start the room's ring: one the room may not start yet is refused before
    // the SFU is asked.
    if (const auto wait = ringer_.ring_limited(room, waiter.request.user)) {
        --in_flight_;
        finish(waiter.answer,
               {.outcome = CallOutcome::RingLimited, .ticket = std::nullopt, .retry_after = wait});
        return;
    }
    // This ticket may start the room's call: past the cap it is refused before the SFU is asked.
    if (ringer_.full()) {
        ++counters_.busy;
        --in_flight_;
        finish(waiter.answer, {.outcome = CallOutcome::Busy, .ticket = std::nullopt});
        return;
    }
    try {
        messages_.members(
            room, std::nullopt, waiter.kind == CallKind::Group ? kGroupMembersRead : kMembersRead,
            [this, room, waiter = std::move(waiter)](
                core::ports::MessageResult<std::vector<core::UserId>> members) mutable noexcept {
                listed(room, std::move(waiter), std::move(members));
            });
    } catch (const std::bad_alloc&) {
        // Whatever was moved out of the waiter answers nothing; the asker times out.
        --in_flight_;
        if (waiter.answer) {
            waiter.answer(std::unexpected(rt::RouteError::Unavailable));
        }
    }
}

void CallHandler::listed(const core::RoomId& room, Waiter waiter,
                         core::ports::MessageResult<std::vector<core::UserId>> members) noexcept {
    if (!members) {
        ++counters_.store_unavailable;
        --in_flight_;
        finish(waiter.answer, {.outcome = CallOutcome::Unavailable, .ticket = std::nullopt});
        return;
    }
    waiter.members = std::move(*members);
    admit(room, std::move(waiter));
}

void CallHandler::admit(const core::RoomId& room, Waiter waiter) noexcept {
    try {
        auto it = rooms_.find(room);
        if (it == rooms_.end()) {
            if (rooms_.size() >= limits_.max_rooms) {
                sweep_now();
            }
            if (rooms_.size() >= limits_.max_rooms) {
                ++counters_.busy;
                --in_flight_;
                finish(waiter.answer, {.outcome = CallOutcome::Busy, .ticket = std::nullopt});
                return;
            }
            it = rooms_.try_emplace(room).first;
        }
        Entry& entry = it->second;
        entry.kind = waiter.kind;
        entry.used = clock_.now();
        entry.waiting.push_back(std::move(waiter));
    } catch (const std::bad_alloc&) {
        // Before the waiter was queued: it is still this call's to answer.
        --in_flight_;
        if (waiter.answer) {
            waiter.answer(std::unexpected(rt::RouteError::Unavailable));
        }
        return;
    }
    pump(room);
}

void CallHandler::enqueue(const core::RoomId& room, CallKind kind, Move move) noexcept {
    try {
        auto it = rooms_.find(room);
        if (it == rooms_.end()) {
            it = rooms_.try_emplace(room).first;
            it->second.kind = kind;
            it->second.used = clock_.now();
        }
        it->second.moves.push_back(std::move(move));
    } catch (const std::bad_alloc&) {
        if (move.answer) {
            move.answer(std::unexpected(rt::RouteError::Unavailable));
        }
        return;
    }
    pump(room);
}

void CallHandler::pump(const core::RoomId& room) noexcept {
    const auto it = rooms_.find(room);
    if (it == rooms_.end()) {
        return;
    }
    Entry& entry = it->second;
    if (entry.busy || clock_.now() < entry.stalled_until) {
        return;
    }
    const auto owner = plane_.owner_generation(room);
    if (!owner) {
        drop(room);
        return;
    }
    // A room taken again after another owner held it may have moved on meanwhile: what this
    // node knew of its generation holds only under the ownership it was read under.
    if (entry.generation == 0 || entry.owner_generation != *owner) {
        entry.busy = true;
        entry.owner_generation = *owner;
        try {
            plane_.media_generation(room, rt::MediaStep::Read,
                                    [this, alive = std::weak_ptr<int>(alive_), room](
                                        rt::StoreResult<std::optional<std::uint64_t>> r) noexcept {
                                        if (!alive.expired()) {
                                            read(room, r);
                                        }
                                    });
        } catch (const std::bad_alloc&) {
            read(room, std::unexpected(rt::StoreError::Unavailable));
        }
        return;
    }
    if (!entry.moves.empty()) {
        entry.busy = true;
        try {
            plane_.media_generation(room, rt::MediaStep::Advance,
                                    [this, alive = std::weak_ptr<int>(alive_), room](
                                        rt::StoreResult<std::optional<std::uint64_t>> r) noexcept {
                                        if (!alive.expired()) {
                                            advanced(room, r);
                                        }
                                    });
        } catch (const std::bad_alloc&) {
            advanced(room, std::unexpected(rt::StoreError::Unavailable));
        }
        return;
    }
    if (entry.waiting.empty()) {
        return;
    }
    if (!entry.media) {
        entry.busy = true;
        const std::uint64_t generation = entry.generation;
        try {
            sfu_->open_room(
                room, core::ports::MediaGeneration{generation}, core::ports::MediaRoomKind::Call,
                cap(entry.kind),
                [this, room, generation](
                    std::expected<std::unique_ptr<core::ports::IMediaRoom>, core::ports::MediaError>
                        result) noexcept { opened(room, generation, std::move(result)); });
        } catch (const std::bad_alloc&) {
            entry.busy = false;
            // Nothing will answer the waiters, these or any that would join them later.
            abandon_open(room);
        }
        return;
    }
    std::vector<Waiter> waiting = std::exchange(entry.waiting, {});
    for (Waiter& w : waiting) {
        // A join may answer inside the call (a participant count known at once): the entry is
        // looked up again for each.
        const auto again = rooms_.find(room);
        if (again == rooms_.end() || !again->second.media) {
            --in_flight_;
            finish(w.answer, {.outcome = CallOutcome::Unavailable, .ticket = std::nullopt});
            continue;
        }
        join(room, again->second, std::move(w));
    }
}

void CallHandler::read(const core::RoomId& room,
                       rt::StoreResult<std::optional<std::uint64_t>> result) noexcept {
    const auto it = rooms_.find(room);
    if (it == rooms_.end()) {
        return;
    }
    Entry& entry = it->second;
    entry.busy = false;
    if (!result) {
        ++counters_.store_unavailable;
        entry.generation = 0;
        entry.stalled_until = clock_.now() + kRetryEvery;
        fail_waiting(entry, CallOutcome::Unavailable);
        return;
    }
    if (!*result) {
        drop(room);
        return;
    }
    if (**result != entry.generation) {
        // Another owner moved it on since this handle was opened, and closed the old one: let go
        // of it, unclosed.
        entry.media.reset();
        entry.generation = **result;
    }
    pump(room);
}

void CallHandler::advanced(const core::RoomId& room,
                           rt::StoreResult<std::optional<std::uint64_t>> result) noexcept {
    const auto it = rooms_.find(room);
    if (it == rooms_.end()) {
        return;
    }
    Entry& entry = it->second;
    entry.busy = false;
    if (entry.moves.empty()) {
        pump(room);
        return;
    }
    if (!result) {
        // Moved or not, nobody knows: the next try moves it again, which only skips a number.
        ++counters_.moves_unavailable;
        entry.stalled_until = clock_.now() + kRetryEvery;
        fail_waiting(entry, CallOutcome::Unavailable);
        return;
    }
    if (!*result) {
        ++counters_.moves_fenced;
        drop(room);
        return;
    }
    Move move = std::move(entry.moves.front());
    entry.moves.pop_front();
    const std::uint64_t old = entry.generation;
    entry.generation = **result;
    if (!move.answer) {
        ++counters_.moves_removal;
    } else if (move.step == MediaStepNeeded::Close) {
        ++counters_.moves_end;
    } else {
        ++counters_.moves_expel;
    }
    // The SFU hears of it only now that the write is done: the old generation closes once its
    // joins are answered, and the next ask opens the new one. Nobody hears of the move until
    // the SFU says the old room is gone: a client that acts on call_moved (the one put out
    // trying its old credential among them) then finds nothing to join.
    retire(room, old, std::move(entry.media), std::exchange(entry.joining, 0), std::move(move));
    pump(room);
}

void CallHandler::announce(const core::RoomId& room, Move& move) noexcept {
    if (move.call && move.step == MediaStepNeeded::Close) {
        ringer_.ended(room, *move.call, move.by);
    } else if (move.call && move.subject) {
        ringer_.moved(room, *move.call, move.by, *move.subject);
    }
    if (move.answer) {
        finish(move.answer, {.outcome = CallOutcome::Done,
                             .ticket = std::nullopt,
                             .call = move.call,
                             .caller = move.by});
    }
}

void CallHandler::announce_retired(Retired& retired) noexcept {
    if (!retired.pending) {
        return;
    }
    Move move = std::move(*retired.pending);
    retired.pending.reset();
    const core::RoomId room = retired.room;
    announce(room, move);
}

void CallHandler::fail_waiting(Entry& entry, CallOutcome outcome) noexcept {
    std::vector<Waiter> waiting = std::exchange(entry.waiting, {});
    for (Waiter& w : waiting) {
        --in_flight_;
        finish(w.answer, {.outcome = outcome, .ticket = std::nullopt});
    }
    std::erase_if(entry.moves, [this](Move& m) {
        if (!m.answer) {
            return false;
        }
        finish(m.answer, {.outcome = CallOutcome::Unavailable, .ticket = std::nullopt});
        return true;
    });
}

void CallHandler::drop(const core::RoomId& room) noexcept {
    const auto it = rooms_.find(room);
    if (it == rooms_.end()) {
        return;
    }
    Entry gone = std::move(it->second);
    rooms_.erase(it);
    for (Waiter& w : gone.waiting) {
        --in_flight_;
        finish(w.answer, {.outcome = CallOutcome::Unavailable, .ticket = std::nullopt});
    }
    for (Move& m : gone.moves) {
        if (m.answer) {
            finish(m.answer, {.outcome = CallOutcome::Unavailable, .ticket = std::nullopt});
        }
    }
}

void CallHandler::retire(const core::RoomId& room, std::uint64_t generation,
                         std::unique_ptr<core::ports::IMediaRoom> media, std::size_t joining,
                         Move move) noexcept {
    const core::MonoTime now = clock_.now();
    try {
        retired_.push_back(Retired{.id = ++next_retired_,
                                   .room = room,
                                   .generation = generation,
                                   .media = std::move(media),
                                   .joining = joining,
                                   .closing = false,
                                   .next_try = now,
                                   .give_up = now + limits_.close_retry_for,
                                   .pending = std::nullopt});
        retired_.back().pending.emplace(std::move(move));
    } catch (const std::bad_alloc&) {
        // Unclosed: whoever is still in it stays until they leave, as before expulsion existed.
        ++counters_.retired_abandoned;
        announce(room, move);
        return;
    }
    close_retired();
}

void CallHandler::close_retired() noexcept {
    const core::MonoTime now = clock_.now();
    // Ids, not references: a close may answer inside the call and change the list.
    std::vector<std::uint64_t> due;
    try {
        for (const Retired& r : retired_) {
            if (!r.closing && r.joining == 0 && now >= r.next_try) {
                due.push_back(r.id);
            }
        }
    } catch (const std::bad_alloc&) {
        return;
    }
    for (const std::uint64_t id : due) {
        const auto it = std::ranges::find(retired_, id, &Retired::id);
        if (it == retired_.end()) {
            continue;
        }
        if (now >= it->give_up) {
            ++counters_.retired_abandoned;
            Retired gone = std::move(*it);
            retired_.erase(it);
            announce_retired(gone);
            continue;
        }
        it->closing = true;
        try {
            if (it->media) {
                it->media->close(
                    [this, id](std::expected<void, core::ports::MediaError> r) noexcept {
                        retired_closed(id, r);
                    });
                continue;
            }
            // Opened elsewhere: a handle is needed to close it, and opening is idempotent.
            sfu_->open_room(
                it->room, core::ports::MediaGeneration{it->generation},
                core::ports::MediaRoomKind::Call, 0,
                [this, id](
                    std::expected<std::unique_ptr<core::ports::IMediaRoom>, core::ports::MediaError>
                        opened) noexcept {
                    const auto r = std::ranges::find(retired_, id, &Retired::id);
                    if (r == retired_.end()) {
                        return;
                    }
                    if (!opened) {
                        retired_closed(id, std::unexpected(opened.error()));
                        return;
                    }
                    r->media = std::move(*opened);
                    r->media->close(
                        [this, id](std::expected<void, core::ports::MediaError> c) noexcept {
                            retired_closed(id, c);
                        });
                });
        } catch (const std::bad_alloc&) {
            it->closing = false;
            it->next_try = now + kRetryEvery;
        }
    }
}

void CallHandler::retired_closed(std::uint64_t id,
                                 std::expected<void, core::ports::MediaError> r) noexcept {
    const auto it = std::ranges::find(retired_, id, &Retired::id);
    if (it == retired_.end()) {
        return;
    }
    if (r || r.error() != core::ports::MediaError::Unavailable) {
        // Closed, or refused as asked, which no retry changes.
        if (r) {
            ++counters_.retired_closed;
        } else {
            ++counters_.retired_abandoned;
        }
        Retired gone = std::move(*it);
        retired_.erase(it);
        announce_retired(gone);
        return;
    }
    ++counters_.sfu_unavailable;
    it->closing = false;
    it->next_try = clock_.now() + kRetryEvery;
    // The SFU could not say: the others are not kept waiting on it, and the close is tried again
    // every second.
    announce_retired(*it);
}

void CallHandler::abandon_open(const core::RoomId& room) noexcept {
    const auto it = rooms_.find(room);
    if (it == rooms_.end()) {
        return;
    }
    std::vector<Waiter> waiting = std::exchange(it->second.waiting, {});
    if (!it->second.media && it->second.joining == 0 && it->second.moves.empty()) {
        rooms_.erase(it);
    }
    for (Waiter& w : waiting) {
        --in_flight_;
        w.answer(std::unexpected(rt::RouteError::Unavailable));
    }
}

void CallHandler::opened(
    const core::RoomId& room, std::uint64_t generation,
    std::expected<std::unique_ptr<core::ports::IMediaRoom>, core::ports::MediaError>
        result) noexcept {
    const auto it = rooms_.find(room);
    if (it == rooms_.end() || it->second.generation != generation) {
        return;
    }
    Entry& entry = it->second;
    entry.busy = false;
    if (!result) {
        const CallOutcome outcome = failure(result.error());
        std::vector<Waiter> waiting = std::exchange(entry.waiting, {});
        if (!entry.media && entry.joining == 0 && entry.moves.empty()) {
            rooms_.erase(it);
        }
        for (Waiter& w : waiting) {
            --in_flight_;
            finish(w.answer, {.outcome = outcome, .ticket = std::nullopt});
        }
        pump(room);
        return;
    }
    ++counters_.opens;
    entry.media = std::move(*result);
    pump(room);
}

void CallHandler::join(const core::RoomId& room, Entry& entry, Waiter waiter) noexcept {
    if (entry.kind != CallKind::Group) {
        issue(room, entry, std::move(waiter));
        return;
    }
    // A group call's cap is the SFU room's, which refuses a device past it only once the client
    // connects: counted here first, so that the asker hears why.
    ++entry.joining;
    const std::uint64_t generation = entry.generation;
    try {
        entry.media->participants(
            [this, room, generation, waiter = std::move(waiter)](
                std::expected<std::vector<core::ports::MediaParticipant>, core::ports::MediaError>
                    listed) mutable noexcept {
                counted(room, generation, std::move(waiter), std::move(listed));
            });
    } catch (const std::bad_alloc&) {
        --entry.joining;
        --in_flight_;
        if (waiter.answer) {
            waiter.answer(std::unexpected(rt::RouteError::Unavailable));
        }
    }
}

void CallHandler::counted(
    const core::RoomId& room, std::uint64_t generation, Waiter waiter,
    std::expected<std::vector<core::ports::MediaParticipant>, core::ports::MediaError>
        listed) noexcept {
    joined(room, generation);
    const auto it = rooms_.find(room);
    if (it == rooms_.end() || it->second.generation != generation || !it->second.media) {
        // Moved on while counting: asked again, the ticket is for the new generation.
        --in_flight_;
        finish(waiter.answer, {.outcome = CallOutcome::Unavailable, .ticket = std::nullopt});
        return;
    }
    Entry& entry = it->second;
    if (listed) {
        const auto others = std::ranges::count_if(*listed, [&](const auto& p) {
            return p.user != waiter.request.user || p.device != waiter.request.device;
        });
        if (static_cast<std::size_t>(others) >= cap(entry.kind)) {
            ++counters_.full;
            --in_flight_;
            finish(waiter.answer, {.outcome = CallOutcome::Full, .ticket = std::nullopt});
            return;
        }
    }
    // Not counted (the SFU did not answer): the SFU's own cap still holds at the connect.
    issue(room, entry, std::move(waiter));
}

void CallHandler::issue(const core::RoomId& room, Entry& entry, Waiter waiter) noexcept {
    try {
        ++entry.joining;
        const core::UserId user = waiter.request.user;
        const core::DeviceId device = waiter.request.device;
        const std::uint64_t generation = entry.generation;
        entry.media->join(user, device, core::ports::MediaRole::Member,
                          [this, room, generation, waiter = std::move(waiter)](
                              std::expected<core::ports::MediaTicket, core::ports::MediaError>
                                  ticket) mutable noexcept {
                              --in_flight_;
                              joined(room, generation);
                              if (!ticket) {
                                  finish(waiter.answer, {.outcome = failure(ticket.error()),
                                                         .ticket = std::nullopt});
                                  return;
                              }
                              const auto it = rooms_.find(room);
                              if (it == rooms_.end() || it->second.generation != generation) {
                                  // A ticket for a generation moved away from admits nobody:
                                  // asked again, it is for the new one.
                                  finish(waiter.answer, {.outcome = CallOutcome::Unavailable,
                                                         .ticket = std::nullopt});
                                  return;
                              }
                              ticketed(room, waiter, waiter.answer, std::move(*ticket));
                          });
    } catch (const std::bad_alloc&) {
        --entry.joining;
        --in_flight_;
        if (waiter.answer) {
            waiter.answer(std::unexpected(rt::RouteError::Unavailable));
        }
    }
}

void CallHandler::joined(const core::RoomId& room, std::uint64_t generation) noexcept {
    if (const auto it = rooms_.find(room);
        it != rooms_.end() && it->second.generation == generation) {
        --it->second.joining;
        return;
    }
    const auto r = std::ranges::find_if(
        retired_, [&](const Retired& x) { return x.room == room && x.generation == generation; });
    if (r != retired_.end() && r->joining > 0 && --r->joining == 0) {
        close_retired();
    }
}

void CallHandler::ticketed(const core::RoomId& room, const Waiter& waiter, rt::OwnerAnswer& answer,
                           core::ports::MediaTicket ticket) noexcept {
    // Rung once the ticket is in hand: a caller the SFU turned away rings nobody.
    const auto call = ringer_.ticketed(room, waiter.request.user, waiter.members, waiter.kind);
    if (!call) {
        switch (call.error().why) {
        case RingRefusal::Why::Busy:
            ++counters_.busy;
            finish(answer, {.outcome = CallOutcome::Busy, .ticket = std::nullopt});
            return;
        case RingRefusal::Why::Limited:
            finish(answer, {.outcome = CallOutcome::RingLimited,
                            .ticket = std::nullopt,
                            .retry_after = call.error().retry_after});
            return;
        case RingRefusal::Why::Expelled:
            ++counters_.expelled;
            finish(answer, {.outcome = CallOutcome::Expelled, .ticket = std::nullopt});
            return;
        }
        return;
    }
    ++counters_.tickets;
    finish(answer, {.outcome = CallOutcome::Ticket, .ticket = std::move(ticket), .call = *call});
}

void CallHandler::finish(rt::OwnerAnswer& answer, const CallAnswer& outcome) noexcept {
    try {
        answer(encode_answer(outcome));
    } catch (const std::bad_alloc&) {
        answer(std::unexpected(rt::RouteError::Unavailable));
    }
}

CallOutcome CallHandler::failure(core::ports::MediaError error) noexcept {
    switch (error) {
    case core::ports::MediaError::Unavailable:
        ++counters_.sfu_unavailable;
        return CallOutcome::Unavailable;
    case core::ports::MediaError::Refused:
    case core::ports::MediaError::Closed:
    case core::ports::MediaError::NotImplemented:
        ++counters_.sfu_refused;
        return CallOutcome::Failed;
    }
    ++counters_.sfu_refused;
    return CallOutcome::Failed;
}

void CallHandler::on_member_removed(const core::RoomId& room, const core::UserId& user) noexcept {
    if (sfu_ == nullptr || !plane_.owns(room)) {
        return;
    }
    const auto call = ringer_.call_of(room);
    const auto entry = rooms_.find(room);
    if (!call) {
        // No call this node knows of; one it ticketed lately may still hold the member.
        if (entry != rooms_.end() && entry->second.media) {
            move_if_connected(room, entry->second.kind,
                              Move{.step = MediaStepNeeded::None,
                                   .call = std::nullopt,
                                   .by = std::nullopt,
                                   .subject = user,
                                   .answer = nullptr});
        }
        return;
    }
    const CallKind kind = ringer_.kind_of(room).value_or(CallKind::Direct);
    const MediaStepNeeded step = ringer_.removed(room, user);
    switch (step) {
    case MediaStepNeeded::None:
        move_if_connected(room, kind,
                          Move{.step = step,
                               .call = *call,
                               .by = std::nullopt,
                               .subject = user,
                               .answer = nullptr});
        return;
    case MediaStepNeeded::Move:
    case MediaStepNeeded::Close:
        enqueue(room, kind,
                Move{.step = step,
                     .call = *call,
                     .by = std::nullopt,
                     .subject = user,
                     .answer = nullptr});
        return;
    }
}

void CallHandler::on_members_resync() noexcept {
    if (sfu_ == nullptr) {
        return;
    }
    try {
        for (const core::RoomId& room : ringer_.rooms()) {
            for (const core::UserId& user : ringer_.joined(room)) {
                ++counters_.resync_checks;
                messages_.access(
                    room, user,
                    [this, room,
                     user](core::ports::MessageResult<core::ports::RoomAccess> access) noexcept {
                        if (access && !access->member) {
                            on_member_removed(room, user);
                        }
                    });
            }
        }
    } catch (const std::bad_alloc&) {
        // Those asked so far are checked; a removal among the rest stays in until it leaves.
        ++counters_.store_unavailable;
    }
}

void CallHandler::check_occupancy() noexcept {
    std::vector<std::pair<core::RoomId, CallId>> due;
    try {
        due = ringer_.take_checks();
    } catch (const std::bad_alloc&) {
        return;
    }
    for (const auto& [room, call] : due) {
        const auto it = rooms_.find(room);
        if (it == rooms_.end() || !it->second.media || it->second.busy) {
            // Nothing to ask with yet: asked again at the next check.
            ++counters_.occupancy_unavailable;
            ringer_.occupied(room, call, std::nullopt);
            continue;
        }
        ++counters_.occupancy_checks;
        try {
            it->second.media->participants(
                [this, room, call](std::expected<std::vector<core::ports::MediaParticipant>,
                                                 core::ports::MediaError>
                                       listed) noexcept {
                    if (!listed) {
                        ++counters_.occupancy_unavailable;
                        ringer_.occupied(room, call, std::nullopt);
                        return;
                    }
                    ringer_.occupied(room, call, !listed->empty());
                });
        } catch (const std::bad_alloc&) {
            ringer_.occupied(room, call, std::nullopt);
        }
    }
}

void CallHandler::sweep() noexcept {
    ringer_.tick();
    check_occupancy();
    // Called after every turn of the loop; the rooms are looked at once a second.
    constexpr core::Millis kSweepEvery{1'000};
    const core::MonoTime now = clock_.now();
    if (now < next_sweep_) {
        return;
    }
    next_sweep_ = now + kSweepEvery;
    sweep_now();
    close_retired();
    // Removals' moves the store could not take, tried again.
    std::vector<core::RoomId> stalled;
    try {
        for (const auto& [room, entry] : rooms_) {
            if (!entry.busy && entry.stalled_until != core::MonoTime{} &&
                now >= entry.stalled_until && (!entry.moves.empty() || !entry.waiting.empty())) {
                stalled.push_back(room);
            }
        }
    } catch (const std::bad_alloc&) {
        return;
    }
    for (const core::RoomId& room : stalled) {
        pump(room);
    }
}

void CallHandler::sweep_now() noexcept {
    const core::MonoTime now = clock_.now();
    std::erase_if(rooms_, [&](const auto& entry) {
        const Entry& e = entry.second;
        // A group call's handle is kept while the call lasts: its occupancy is asked through it.
        const bool group_call = e.kind == CallKind::Group && !ringer_.idle(entry.first);
        return !e.busy && e.waiting.empty() && e.moves.empty() && e.joining == 0 && !group_call &&
               now - e.used >= limits_.idle;
    });
}

} // namespace chat
