#include "call.hpp"

#include "layout.hpp"

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

// A direct chat lists two members; a few more are read, for a list that grew.
constexpr std::size_t kMembersRead = 8;

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
    return out;
}

std::optional<CallAsk> decode_request(std::span<const std::byte> bytes) {
    layout::Reader in{bytes};
    if (in.u8() != kLayout) {
        return std::nullopt;
    }
    const auto what = in.u8();
    auto user = in.user();
    if (!what || !user || *what > static_cast<std::uint8_t>(CallSignal::End)) {
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
    if (!call || !in.empty()) {
        return std::nullopt;
    }
    return CallSignalRequest{
        .user = *user, .signal = static_cast<CallSignal>(*what), .call = *call};
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
    if (!outcome || *outcome > static_cast<std::uint8_t>(CallOutcome::RingLimited)) {
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
                         IRingPlane& plane, const core::ports::IClock& clock,
                         core::ports::IRandom& random, CallLimits limits)
    : messages_(messages), sfu_(sfu), clock_(clock), limits_(limits),
      ringer_(plane, clock, random, limits.ring), next_sweep_(clock.now()) {}

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

bool CallHandler::callable(const core::ports::MessageResult<core::ports::RoomAccess>& access,
                           rt::OwnerAnswer& answer) noexcept {
    if (!access) {
        ++counters_.store_unavailable;
        finish(answer, {.outcome = CallOutcome::Unavailable, .ticket = std::nullopt});
        return false;
    }
    if (access->kind != core::ports::RoomKind::DirectChat) {
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
    if (!ringer_.idle(room)) {
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
            room, std::nullopt, kMembersRead,
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
        entry.used = clock_.now();
        if (entry.media) {
            join(room, entry, std::move(waiter));
            return;
        }
        // Opening already: this ask waits for it, and the room is opened once however many
        // asked meanwhile.
        const bool first = entry.opening.empty();
        entry.opening.push_back(std::move(waiter));
        if (!first) {
            return;
        }
        try {
            sfu_->open_room(
                room, kCallGeneration, core::ports::MediaRoomKind::Call, kCallParticipants,
                [this, room](
                    std::expected<std::unique_ptr<core::ports::IMediaRoom>, core::ports::MediaError>
                        result) noexcept { opened(room, std::move(result)); });
        } catch (const std::bad_alloc&) {
            // Nothing will answer the waiters, this one or any that would join them later.
            abandon_open(room);
        }
    } catch (const std::bad_alloc&) {
        // Before the waiter was queued: it is still this call's to answer.
        --in_flight_;
        if (waiter.answer) {
            waiter.answer(std::unexpected(rt::RouteError::Unavailable));
        }
    }
}

void CallHandler::abandon_open(const core::RoomId& room) noexcept {
    const auto it = rooms_.find(room);
    if (it == rooms_.end()) {
        return;
    }
    std::vector<Waiter> waiting = std::exchange(it->second.opening, {});
    if (!it->second.media && it->second.joining == 0) {
        rooms_.erase(it);
    }
    for (Waiter& w : waiting) {
        --in_flight_;
        w.answer(std::unexpected(rt::RouteError::Unavailable));
    }
}

void CallHandler::opened(
    const core::RoomId& room,
    std::expected<std::unique_ptr<core::ports::IMediaRoom>, core::ports::MediaError>
        result) noexcept {
    const auto it = rooms_.find(room);
    if (it == rooms_.end()) {
        return;
    }
    Entry& entry = it->second;
    std::vector<Waiter> waiting = std::exchange(entry.opening, {});
    if (!result) {
        const CallOutcome outcome = failure(result.error());
        for (Waiter& w : waiting) {
            --in_flight_;
            finish(w.answer, {.outcome = outcome, .ticket = std::nullopt});
        }
        if (!entry.media && entry.joining == 0) {
            rooms_.erase(it);
        }
        return;
    }
    ++counters_.opens;
    entry.media = std::move(*result);
    for (Waiter& w : waiting) {
        join(room, entry, std::move(w));
    }
}

void CallHandler::join(const core::RoomId& room, Entry& entry, Waiter waiter) noexcept {
    try {
        ++entry.joining;
        const core::UserId user = waiter.request.user;
        const core::DeviceId device = waiter.request.device;
        entry.media->join(user, device, core::ports::MediaRole::Member,
                          [this, room, waiter = std::move(waiter)](
                              std::expected<core::ports::MediaTicket, core::ports::MediaError>
                                  ticket) mutable noexcept {
                              --in_flight_;
                              if (const auto it = rooms_.find(room); it != rooms_.end()) {
                                  --it->second.joining;
                              }
                              if (!ticket) {
                                  finish(waiter.answer, {.outcome = failure(ticket.error()),
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

void CallHandler::ticketed(const core::RoomId& room, const Waiter& waiter, rt::OwnerAnswer& answer,
                           core::ports::MediaTicket ticket) noexcept {
    // Rung once the ticket is in hand: a caller the SFU turned away rings nobody.
    const auto call = ringer_.ticketed(room, waiter.request.user, waiter.members);
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

void CallHandler::sweep() noexcept {
    ringer_.tick();
    // Called after every turn of the loop; the rooms are looked at once a second.
    constexpr core::Millis kSweepEvery{1'000};
    const core::MonoTime now = clock_.now();
    if (now < next_sweep_) {
        return;
    }
    next_sweep_ = now + kSweepEvery;
    sweep_now();
}

void CallHandler::sweep_now() noexcept {
    const core::MonoTime now = clock_.now();
    std::erase_if(rooms_, [&](const auto& entry) {
        const Entry& e = entry.second;
        return e.opening.empty() && e.joining == 0 && now - e.used >= limits_.idle;
    });
}

} // namespace chat
