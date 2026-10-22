#include "call.hpp"

#include "layout.hpp"
#include "log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chat {

namespace {

// The layouts' first byte; a node that reads another is answered Unavailable, as for an owner
// that cannot be reached. 2: an ask says what it asks for, and a ticket names its call. 3: a
// ticket carries the relays its client reaches the media server through (ADR-0109), so a node
// that would drop them refuses the answer instead.
constexpr std::uint8_t kLayout = 4;
// What an ask asks for: a ticket, or one of CallSignal's values, or a ticket answering a call
// it names (after Expel's value, so that a node without it reads it as a layout it does not
// know and answers Unavailable, which the client retries).
constexpr std::uint8_t kTicket = 0;
constexpr std::uint8_t kAnswer = 6;
// A status ask: no user first, as every other kind has, but a flag and then perhaps one. A node
// of an earlier release reads it as malformed and answers Unavailable.
constexpr std::uint8_t kStatus = 7;
// A current-call ask (ADR-0114): the kind alone.
constexpr std::uint8_t kCurrent = 8;
// The most users a snapshot's lists carry, each: a group call's callees and members in it.
constexpr std::size_t kMaxSnapshotUsers = 128;

// A direct chat lists two members; a few more are read, for a list that grew. A group call
// rings at most kMaxGroupCallees others, the caller aside.
constexpr std::size_t kMembersRead = 8;
constexpr std::size_t kGroupMembersRead = kMaxGroupCallees + 1;
// A move asked while this many wait in the room is answered busy: the caller's own, in order.
constexpr std::size_t kMaxMovesWaiting = 16;
// A removal's move the store could not take, and an old media room the SFU could not close,
// are tried again after this.
constexpr core::Millis kRetryEvery{1'000};
// How long a ticket admits its device to the SFU (the LiveKit adapter's kTicketTtl): a device
// ticketed within this may be connected before the SFU lists it.
constexpr core::Millis kTicketLifetime{60'000};
// Tickets remembered per generation, for the above.
constexpr std::size_t kMaxTicketed = 64;

// How a refused ticket reads in the log: the error the client is sent.
std::string_view refusal_name(CallOutcome outcome) noexcept {
    switch (outcome) {
    case CallOutcome::NotMember:
        return "not_member";
    case CallOutcome::NotCallable:
        return "not_callable";
    case CallOutcome::Unavailable:
        return "unavailable";
    case CallOutcome::Failed:
        return "call_failed";
    case CallOutcome::Disabled:
        return "calls_disabled";
    case CallOutcome::Busy:
        return "busy";
    case CallOutcome::NoCall:
        return "no_call";
    case CallOutcome::RingLimited:
        return "ring_limited";
    case CallOutcome::Expelled:
        return "expelled";
    case CallOutcome::Full:
        return "call_full";
    default:
        return "other";
    }
}

// Between a close's tries: 1 s, 2 s, 4 s and so on after the first, 30 s at most.
core::Millis backoff(std::uint32_t attempt) noexcept {
    const std::uint32_t doublings = std::min<std::uint32_t>(attempt == 0 ? 0 : attempt - 1, 5);
    return std::min(kRetryEvery * (1U << doublings), core::Millis{30'000});
}

// A ticket's relays: their count, and each one's URIs, username and credential. A list longer
// than the counts hold is refused, never cut: a client given part of it may not reach the
// media server.
bool put_ice_servers(std::vector<std::byte>& out,
                     const std::vector<core::ports::IceServer>& servers) {
    constexpr std::size_t kMaxCount = std::numeric_limits<std::uint16_t>::max();
    if (servers.size() > kMaxCount) {
        return false;
    }
    layout::put_u16(out, static_cast<std::uint16_t>(servers.size()));
    for (const core::ports::IceServer& server : servers) {
        if (server.urls.size() > kMaxCount || server.username.size() > kMaxCount ||
            server.credential.size() > kMaxCount) {
            return false;
        }
        layout::put_u16(out, static_cast<std::uint16_t>(server.urls.size()));
        for (const std::string& url : server.urls) {
            if (url.size() > kMaxCount) {
                return false;
            }
            layout::put_long(out, url);
        }
        layout::put_long(out, server.username);
        layout::put_long(out, server.credential);
    }
    return true;
}

std::optional<std::vector<core::ports::IceServer>> read_ice_servers(layout::Reader& in) {
    const auto count = in.u16();
    if (!count) {
        return std::nullopt;
    }
    std::vector<core::ports::IceServer> servers;
    for (std::uint16_t i = 0; i < *count; ++i) {
        const auto urls = in.u16();
        if (!urls) {
            return std::nullopt;
        }
        core::ports::IceServer server;
        for (std::uint16_t u = 0; u < *urls; ++u) {
            const auto url = in.long_text();
            if (!url) {
                return std::nullopt;
            }
            server.urls.emplace_back(*url);
        }
        const auto username = in.long_text();
        const auto credential = in.long_text();
        if (!username || !credential) {
            return std::nullopt;
        }
        server.username = std::string(*username);
        server.credential = std::string(*credential);
        servers.push_back(std::move(server));
    }
    return servers;
}

void put_users(std::vector<std::byte>& out, const std::vector<core::UserId>& users) {
    const std::size_t n = std::min(users.size(), kMaxSnapshotUsers);
    layout::put_u16(out, static_cast<std::uint16_t>(n));
    for (std::size_t i = 0; i < n; ++i) {
        layout::put_short(out, users[i].view());
    }
}

std::optional<std::vector<core::UserId>> read_users(layout::Reader& in) {
    const auto n = in.u16();
    if (!n || *n > kMaxSnapshotUsers) {
        return std::nullopt;
    }
    std::vector<core::UserId> users;
    users.reserve(*n);
    for (std::uint16_t i = 0; i < *n; ++i) {
        auto user = in.user();
        if (!user) {
            return std::nullopt;
        }
        users.push_back(*user);
    }
    return users;
}

void put_time(std::vector<std::byte>& out, core::WallTime t) {
    layout::put_u64(
        out, static_cast<std::uint64_t>(std::max<core::Millis::rep>(
                 0, std::chrono::duration_cast<core::Millis>(t.time_since_epoch()).count())));
}

std::optional<core::WallTime> read_time(layout::Reader& in) {
    const auto ms = in.u64();
    if (!ms ||
        *ms > static_cast<std::uint64_t>(std::numeric_limits<core::Millis::rep>::max() / 1'000)) {
        return std::nullopt;
    }
    return core::WallTime{core::Millis{static_cast<core::Millis::rep>(*ms)}};
}

void put_snapshot(std::vector<std::byte>& out, const CallSnapshot& s) {
    layout::put_uuid(out, s.call);
    layout::put_u8(out, s.kind == CallKind::Group ? 1 : 0);
    layout::put_short(out, s.caller.view());
    layout::put_u8(out, s.answered ? 1 : 0);
    put_users(out, s.ringing);
    put_users(out, s.joined);
    layout::put_u8(out, s.connected ? 1 : 0);
    if (s.connected) {
        put_users(out, *s.connected);
    }
    put_time(out, s.started_at);
    layout::put_u8(out, s.answered_at ? 1 : 0);
    if (s.answered_at) {
        put_time(out, *s.answered_at);
    }
    put_time(out, s.expires_at);
}

std::optional<CallSnapshot> read_snapshot(layout::Reader& in) {
    const auto call = in.uuid<CallId>();
    const auto kind = in.u8();
    auto caller = in.user();
    const auto answered = in.u8();
    auto ringing = read_users(in);
    auto joined = read_users(in);
    const auto has_connected = in.u8();
    if (!call || !kind || *kind > 1 || !caller || !answered || *answered > 1 || !ringing ||
        !joined || !has_connected || *has_connected > 1) {
        return std::nullopt;
    }
    std::optional<std::vector<core::UserId>> connected;
    if (*has_connected == 1) {
        connected = read_users(in);
        if (!connected) {
            return std::nullopt;
        }
    }
    const auto started_at = read_time(in);
    const auto has_answered_at = in.u8();
    if (!started_at || !has_answered_at || *has_answered_at > 1) {
        return std::nullopt;
    }
    std::optional<core::WallTime> answered_at;
    if (*has_answered_at == 1) {
        answered_at = read_time(in);
        if (!answered_at) {
            return std::nullopt;
        }
    }
    const auto expires_at = read_time(in);
    if (!expires_at) {
        return std::nullopt;
    }
    return CallSnapshot{.call = *call,
                        .kind = *kind == 1 ? CallKind::Group : CallKind::Direct,
                        .caller = *caller,
                        .answered = *answered == 1,
                        .ringing = std::move(*ringing),
                        .joined = std::move(*joined),
                        .connected = std::move(connected),
                        .started_at = *started_at,
                        .answered_at = answered_at,
                        .expires_at = *expires_at};
}

} // namespace

using layout::put_long;
using layout::put_short;
using layout::put_u64;
using layout::put_u8;
using layout::put_uuid;

std::vector<std::byte> encode_request(const CallRequest& request) {
    std::vector<std::byte> out;
    put_u8(out, kLayout);
    put_u8(out, request.answering ? kAnswer : kTicket);
    put_short(out, request.user.view());
    put_uuid(out, request.device);
    if (request.answering) {
        put_uuid(out, *request.answering);
    }
    // How it rings (layout 4, ADR-0115).
    put_u8(out, static_cast<std::uint8_t>(request.ring.mode));
    put_users(out, request.ring.only);
    layout::put_u16(out, request.ring.max_people);
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
    // Why a decline was given (layout 4, ADR-0115).
    if (request.signal == CallSignal::Decline) {
        put_u8(out, request.reason ? static_cast<std::uint8_t>(*request.reason) : 0);
        put_long(out, std::string_view(request.text).substr(0, kMaxDeclineText));
    }
    return out;
}

namespace {

// A status ask past its kind: the call, then a flag and perhaps a user.
std::optional<CallAsk> decode_status(layout::Reader& in) {
    const auto call = in.uuid<CallId>();
    const auto has_user = in.u8();
    if (!call || !has_user || *has_user > 1) {
        return std::nullopt;
    }
    std::optional<core::UserId> user;
    if (*has_user == 1) {
        user = in.user();
        if (!user) {
            return std::nullopt;
        }
    }
    if (!in.empty()) {
        return std::nullopt;
    }
    return CallStatusRequest{.call = *call, .user = user};
}

} // namespace

std::vector<std::byte> encode_request(const CallStatusRequest& request) {
    std::vector<std::byte> out;
    put_u8(out, kLayout);
    put_u8(out, kStatus);
    put_uuid(out, request.call);
    put_u8(out, request.user ? 1 : 0);
    if (request.user) {
        put_short(out, request.user->view());
    }
    return out;
}

std::vector<std::byte> encode_request(const CallCurrentRequest& /*request*/) {
    std::vector<std::byte> out;
    put_u8(out, kLayout);
    put_u8(out, kCurrent);
    return out;
}

std::optional<CallAsk> decode_request(std::span<const std::byte> bytes) {
    layout::Reader in{bytes};
    if (in.u8() != kLayout) {
        return std::nullopt;
    }
    const auto what = in.u8();
    if (what == kStatus) {
        return decode_status(in);
    }
    if (what == kCurrent) {
        if (!in.empty()) {
            return std::nullopt;
        }
        return CallCurrentRequest{};
    }
    auto user = in.user();
    if (!what || !user ||
        (*what > static_cast<std::uint8_t>(CallSignal::Expel) && *what != kAnswer)) {
        return std::nullopt;
    }
    if (*what == kTicket || *what == kAnswer) {
        const auto device = in.uuid<core::DeviceId>();
        if (!device) {
            return std::nullopt;
        }
        std::optional<CallId> answering;
        if (*what == kAnswer) {
            answering = in.uuid<CallId>();
            if (!answering) {
                return std::nullopt;
            }
        }
        const auto mode = in.u8();
        auto only = read_users(in);
        const auto max_people = in.u16();
        if (!mode || *mode > static_cast<std::uint8_t>(RingOptions::Mode::None) || !only ||
            !max_people || !in.empty()) {
            return std::nullopt;
        }
        return CallRequest{.user = *user,
                           .device = *device,
                           .answering = answering,
                           .ring = {.mode = static_cast<RingOptions::Mode>(*mode),
                                    .only = std::move(*only),
                                    .max_people = *max_people}};
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
    std::optional<CallReason> reason;
    std::string text;
    if (signal == CallSignal::Decline) {
        const auto r = in.u8();
        const auto said = in.long_text();
        if (!r || (*r != 0 && !decline_reason(reason_name(static_cast<CallReason>(*r)))) || !said ||
            said->size() > kMaxDeclineText) {
            return std::nullopt;
        }
        if (*r != 0) {
            reason = static_cast<CallReason>(*r);
        }
        text = std::string(*said);
    }
    if (!in.empty()) {
        return std::nullopt;
    }
    return CallSignalRequest{.user = *user,
                             .signal = signal,
                             .call = *call,
                             .target = target,
                             .reason = reason,
                             .text = std::move(text)};
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
        if (!put_ice_servers(out, answer.ticket->ice_servers)) {
            // Unreadable on purpose: the asking node answers its client Unavailable.
            return {};
        }
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
    if (answer.outcome == CallOutcome::Current) {
        if (!answer.snapshot) {
            // Unreadable on purpose, as a ticket's relays that do not fit.
            return {};
        }
        put_snapshot(out, *answer.snapshot);
    }
    return out;
}

std::optional<CallAnswer> decode_answer(std::span<const std::byte> bytes) {
    layout::Reader in{bytes};
    if (in.u8() != kLayout) {
        return std::nullopt;
    }
    const auto outcome = in.u8();
    if (!outcome || *outcome > static_cast<std::uint8_t>(CallOutcome::Current)) {
        return std::nullopt;
    }
    CallAnswer answer{.outcome = static_cast<CallOutcome>(*outcome), .ticket = std::nullopt};
    if (answer.outcome == CallOutcome::Ticket) {
        const auto endpoint = in.long_text();
        const auto credential = in.long_text();
        const auto ms = in.u64();
        auto ice_servers = read_ice_servers(in);
        const auto has_call = in.u8();
        if (!endpoint || !credential || !ms || !ice_servers || !has_call || *has_call > 1) {
            return std::nullopt;
        }
        answer.ticket = core::ports::MediaTicket{
            .endpoint = std::string(*endpoint),
            .credential = std::string(*credential),
            .expires_at = core::WallTime{core::Millis{static_cast<core::Millis::rep>(*ms)}},
            .ice_servers = std::move(*ice_servers)};
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
    if (answer.outcome == CallOutcome::Current) {
        answer.snapshot = read_snapshot(in);
        if (!answer.snapshot) {
            return std::nullopt;
        }
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
    limits_.group_people =
        limits_.group_people == 0
            ? limits_.group_participants
            : std::clamp(limits_.group_people, kMinGroupPeople, kMaxGroupParticipants);
    ringer_.on_group_ended([this](const core::RoomId& room) noexcept { group_ended(room); });
    recorder_.clock = &clock;
    ringer_.watch(&recorder_);
}

void CallHandler::RingRecorder::observed(const CallEvent& event) noexcept {
    if (next != nullptr) {
        next->observed(event);
    }
    if (store == nullptr) {
        return;
    }
    // A ring starts, or ends for everyone it rang. A group call answered by one, or declined by
    // one, rings the others on, until it rings out, ends, or the last of them answers or
    // declines: from then it is on, and a new owner must not end it (ADR-0129).
    const bool direct = event.kind == CallKind::Direct;
    const bool ended =
        event.event == RingEvent::Cancelled || event.event == RingEvent::Missed ||
        event.event == RingEvent::Ended ||
        ((event.event == RingEvent::Answered || event.event == RingEvent::Declined) &&
         (direct || !event.rings));
    auto done = [this](core::ports::RingStoreResult<void> result) noexcept {
        if (!result) {
            ++failed;
        }
    };
    try {
        if (event.event == RingEvent::Ringing) {
            store->put_ring(core::ports::StoredRing{.room = event.room,
                                                    .call = event.call.uuid(),
                                                    .caller = event.caller,
                                                    .group = !direct,
                                                    .callees = event.users,
                                                    .started_at = clock->wall_now(),
                                                    .expires_at = event.expires_at},
                            std::move(done));
        } else if (ended) {
            store->end_ring(event.room, event.call.uuid(), std::move(done));
        }
    } catch (const std::bad_alloc&) {
        ++failed;
    }
}

void CallHandler::took_room(const core::RoomId& room) noexcept {
    core::ports::IRingStore* store = recorder_.store;
    if (store == nullptr || !ringer_.idle(room)) {
        return;
    }
    store->ring_of(
        room,
        [this, alive = std::weak_ptr(alive_),
         room](core::ports::RingStoreResult<std::optional<core::ports::StoredRing>> ring) noexcept {
            if (alive.expired() || !ring || !*ring || !ringer_.idle(room)) {
                return;
            }
            const core::ports::StoredRing& left = **ring;
            const auto call = CallId::parse(left.call.to_string());
            if (!call) {
                return;
            }
            // The call's own row: ending it removes it, as any ring's end does.
            ringer_.end_moved(room, *call, left.group ? CallKind::Group : CallKind::Direct,
                              left.caller, left.callees, left.expires_at);
        });
}

void CallHandler::group_ended(const core::RoomId& room) noexcept {
    if (sfu_ == nullptr || !plane_.owns(room)) {
        return;
    }
    // Queued behind any move still waiting, and ahead of the asks that start the next call.
    enqueue(room, CallKind::Group,
            Move{.step = MediaStepNeeded::None,
                 .call = std::nullopt,
                 .by = std::nullopt,
                 .subject = std::nullopt,
                 .answer = nullptr,
                 .clear = true});
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
        if (const auto* s = std::get_if<CallStatusRequest>(&*asked)) {
            // What the ringer knows now; nothing to wait on.
            const auto ringing = ringer_.ringing(room, s->call, s->user);
            CallOutcome outcome = CallOutcome::NoCall;
            if (ringing) {
                outcome = *ringing ? CallOutcome::Ringing : CallOutcome::Answered;
            }
            finish(answer, {.outcome = outcome, .ticket = std::nullopt});
            return;
        }
        if (std::holds_alternative<CallCurrentRequest>(*asked)) {
            current(room, std::move(answer));
            return;
        }
        ++in_flight_;
        counted = true;
        if (const auto* s = std::get_if<CallSignalRequest>(&*asked)) {
            signal(room, *s, std::move(answer));
            return;
        }
        const auto* ticket = std::get_if<CallRequest>(&*asked);
        if (ticket == nullptr) {
            --in_flight_;
            counted = false;
            answer(std::unexpected(rt::RouteError::Unavailable));
            return;
        }
        // A ticket refused is one warn line on the owner, with why (#192).
        answer = [inner = std::move(answer), room, user = ticket->user, device = ticket->device](
                     std::expected<std::vector<std::byte>, rt::RouteError> r) mutable noexcept {
            try {
                if (r) {
                    if (const auto a = decode_answer(*r); a && a->outcome != CallOutcome::Ticket) {
                        log_event(R"("level":"warn","msg":"call ticket refused","room":"{}",)"
                                  R"("user":"{}","device":"{}","reason":"{}")",
                                  room.to_string(), user.view(), device.to_string(),
                                  refusal_name(a->outcome));
                    }
                }
            } catch (const std::bad_alloc&) {
                // The line is lost, not the answer.
                static_cast<void>(std::fputs("chat_server: log line dropped\n", stderr));
            }
            inner(std::move(r));
        };
        // The member list, read on the owner at the moment of asking: a client's join may be
        // older than a removal still on its way to its node (ADR-0073).
        messages_.access(
            room, ticket->user,
            [this, room, waiter = Waiter{.request = *ticket, .answer = std::move(answer)}](
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
        moderate(room, request, answer, access->role == core::ports::MemberRole::Admin);
        return;
    }
    const auto caller = ringer_.signal(room, request.user, request.signal, request.call,
                                       request.reason, request.text);
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
                           rt::OwnerAnswer& answer, bool admin) noexcept {
    if (request.signal == CallSignal::End || !request.target) {
        moderated(room, request, answer, admin);
        return;
    }
    // Only a member of the chat is put out: anyone else has no call to be in.
    try {
        messages_.access(
            room, *request.target,
            [this, room, request, admin, answer = std::move(answer)](
                core::ports::MessageResult<core::ports::RoomAccess> target) mutable noexcept {
                if (!target) {
                    ++counters_.store_unavailable;
                    finish(answer, {.outcome = CallOutcome::Unavailable, .ticket = std::nullopt});
                    return;
                }
                if (!target->member) {
                    ++counters_.no_call;
                    finish(answer, {.outcome = CallOutcome::NoCall, .ticket = std::nullopt});
                    return;
                }
                moderated(room, request, answer, admin);
            });
    } catch (const std::bad_alloc&) {
        if (answer) {
            answer(std::unexpected(rt::RouteError::Unavailable));
        }
    }
}

void CallHandler::moderated(const core::RoomId& room, const CallSignalRequest& request,
                            rt::OwnerAnswer& answer, bool admin) noexcept {
    const bool end = request.signal == CallSignal::End;
    const auto step = end ? ringer_.may_end(room, request.user, request.call, admin)
                      : request.target
                          ? ringer_.expel(room, request.user, request.call, *request.target, admin)
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
    // The stored list holds kMaxMediaExpelled at most: one more is refused, not lost.
    if (!end && it != rooms_.end() && it->second.expelled.size() >= rt::kMaxMediaExpelled &&
        !expelled_from(it->second, *request.target)) {
        ++counters_.busy;
        finish(answer, {.outcome = CallOutcome::Busy, .ticket = std::nullopt});
        return;
    }
    Move move{.step = *step,
              .call = request.call,
              .by = request.user,
              .subject = request.target,
              .answer = std::move(answer),
              .clear = false};
    if (*step == MediaStepNeeded::None) {
        // No ticket in this call; but a device may still be connected from before this node
        // owned the room, which only the SFU knows.
        move_if_connected(room, CallKind::Group, std::move(move));
        return;
    }
    enqueue(room, CallKind::Group, std::move(move));
}

void CallHandler::move_if_connected(const core::RoomId& room, CallKind kind, Move move) noexcept {
    // An expulsion is kept with the generation even when nothing moves; a removal needs nothing
    // kept, as the member list refuses the user already.
    const bool expulsion = move.by.has_value();
    const auto keep = [this, room, kind, expulsion](Move m) noexcept {
        if (expulsion) {
            m.step = MediaStepNeeded::None;
            enqueue(room, kind, std::move(m));
            return;
        }
        // Nothing of theirs to put out: a later removal of them asks again.
        const auto at = rooms_.find(room);
        if (at != rooms_.end() && m.subject) {
            std::erase(at->second.removing, *m.subject);
        }
    };
    const auto it = rooms_.find(room);
    if (it != rooms_.end() && move.subject && ticketed_lately(it->second, *move.subject)) {
        // A ticket of theirs may still admit a device the SFU does not list yet.
        move.step = MediaStepNeeded::Move;
        enqueue(room, kind, std::move(move));
        return;
    }
    if (it == rooms_.end() || !it->second.media || it->second.busy) {
        // No handle to ask with, and no ticket of theirs lately: nobody of theirs to put out.
        keep(std::move(move));
        return;
    }
    std::shared_ptr<Move> slot;
    try {
        slot = std::make_shared<Move>();
    } catch (const std::bad_alloc&) {
        // Not asked: the asker is told to retry; a removal is found again by a later resync.
        ++counters_.sfu_unavailable;
        if (move.answer) {
            finish(move.answer, {.outcome = CallOutcome::Unavailable, .ticket = std::nullopt});
        }
        return;
    }
    *slot = std::move(move);
    try {
        it->second.media->participants(
            [this, room, kind, slot, keep](
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
                keep(std::move(m));
            });
    } catch (const std::bad_alloc&) {
        ++counters_.sfu_unavailable;
        if (slot->answer) {
            finish(slot->answer, {.outcome = CallOutcome::Unavailable, .ticket = std::nullopt});
        }
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
    // A ticket answering a ring that is over (cancelled while the callee reached for it, or
    // rung out, or a ring the callee's device never heard end) answers nothing: it is refused,
    // rather than starting a ring of its own that the caller, who just gave up, never asked for.
    if (waiter.request.answering && !ringer_.answerable(room, waiter.request.user)) {
        ++counters_.no_call;
        --in_flight_;
        finish(waiter.answer, {.outcome = CallOutcome::NoCall, .ticket = std::nullopt});
        return;
    }
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
    restart(room, std::move(waiter));
}

void CallHandler::restart(const core::RoomId& room, Waiter waiter) noexcept {
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
        // Removals queue no further than asks do: one past that is dropped, and a later resync
        // finds the member again.
        if (!move.answer && it->second.moves.size() >= kMaxMovesWaiting) {
            ++counters_.busy;
            if (move.subject) {
                std::erase(it->second.removing, *move.subject);
            }
            return;
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
    if (entry.generation == 0 || entry.reread || entry.owner_generation != *owner) {
        if (entry.owner_generation != *owner) {
            // Another owner may have ticketed it meanwhile.
            entry.minted = false;
        }
        entry.busy = true;
        entry.owner_generation = *owner;
        try {
            plane_.media_generation(room, rt::MediaChange{.step = rt::MediaStep::Read},
                                    [this, alive = std::weak_ptr<int>(alive_), room](
                                        rt::StoreResult<std::optional<rt::MediaState>> r) noexcept {
                                        if (!alive.expired()) {
                                            read(room, std::move(r));
                                        }
                                    });
        } catch (const std::bad_alloc&) {
            read(room, std::unexpected(rt::StoreError::Unavailable));
        }
        return;
    }
    // A call's end that has nobody put out to clear writes nothing.
    while (!entry.moves.empty() && entry.moves.front().clear && entry.expelled.empty()) {
        entry.moves.pop_front();
    }
    if (!entry.moves.empty()) {
        entry.busy = true;
        try {
            // To put someone out: the generation moves on with the list of who is out of it,
            // or, with no device of theirs in it, stays with them added. To end the call: the
            // next generation starts with nobody out, or, ended without a move, the generation
            // stays with nobody out.
            const Move& next = entry.moves.front();
            const rt::MediaChange change{
                .step = next.step == MediaStepNeeded::None ? rt::MediaStep::Expel
                                                           : rt::MediaStep::Advance,
                .carry = next.step != MediaStepNeeded::Close && !next.clear,
                .expel = next.step == MediaStepNeeded::Close ? std::nullopt : next.subject};
            plane_.media_generation(room, change,
                                    [this, alive = std::weak_ptr<int>(alive_), room](
                                        rt::StoreResult<std::optional<rt::MediaState>> r) noexcept {
                                        if (!alive.expired()) {
                                            advanced(room, std::move(r));
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
        // Put out of this generation, by this owner or one before it.
        if (expelled_from(again->second, w.request.user)) {
            ++counters_.expelled;
            --in_flight_;
            finish(w.answer, {.outcome = CallOutcome::Expelled, .ticket = std::nullopt});
            continue;
        }
        join(room, again->second, std::move(w));
    }
}

void CallHandler::read(const core::RoomId& room,
                       rt::StoreResult<std::optional<rt::MediaState>> result) noexcept {
    const auto it = rooms_.find(room);
    if (it == rooms_.end()) {
        return;
    }
    Entry& entry = it->second;
    entry.busy = false;
    if (!result) {
        ++counters_.store_unavailable;
        entry.reread = true;
        entry.stalled_until = clock_.now() + kRetryEvery;
        fail_waiting(entry, CallOutcome::Unavailable);
        return;
    }
    std::optional<rt::MediaState>& state = *result;
    if (!state) {
        drop(room);
        return;
    }
    if (state->generation != entry.generation) {
        // Another owner moved it on since this handle was opened, and closed the old one: let go
        // of it, unclosed.
        entry.media.reset();
        entry.generation = state->generation;
        entry.minted = false;
        entry.ticketed.clear();
        entry.removing.clear();
    }
    // Who was put out of it stays out, whichever owner put them out.
    entry.expelled = std::move(state->expelled);
    entry.reread = false;
    pump(room);
}

void CallHandler::advanced(const core::RoomId& room,
                           rt::StoreResult<std::optional<rt::MediaState>> result) noexcept {
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
        // Moved or not, nobody knows: the generation is read again, and the move tried again
        // (which at worst skips a number). The asker is told to retry now, within its deadline;
        // the move itself is kept.
        ++counters_.moves_unavailable;
        entry.reread = true;
        entry.stalled_until = clock_.now() + kRetryEvery;
        if (entry.moves.front().answer) {
            finish(entry.moves.front().answer,
                   {.outcome = CallOutcome::Unavailable, .ticket = std::nullopt});
            entry.moves.front().answer = nullptr;
        }
        fail_waiting(entry, CallOutcome::Unavailable);
        return;
    }
    std::optional<rt::MediaState>& written = *result;
    if (!written) {
        ++counters_.moves_fenced;
        drop(room);
        return;
    }
    rt::MediaState state = std::move(*written);
    Move move = std::move(entry.moves.front());
    entry.moves.pop_front();
    entry.expelled = std::move(state.expelled);
    // Answered as soon as it is written: everything after is the SFU's, which may take longer
    // than the asker waits.
    if (move.answer) {
        finish(move.answer, {.outcome = CallOutcome::Done,
                             .ticket = std::nullopt,
                             .call = move.call,
                             .caller = move.by});
        move.answer = nullptr;
    }
    if (move.clear) {
        ++counters_.expulsions_cleared;
        pump(room);
        return;
    }
    if (move.step == MediaStepNeeded::None) {
        // Put out of the generation as it stands: nothing moves, and only they are told.
        ++counters_.expulsions_kept;
        if (move.call && move.subject) {
            ringer_.moved(room, *move.call, move.by, *move.subject, false);
        }
        pump(room);
        return;
    }
    const std::uint64_t old = entry.generation;
    entry.generation = state.generation;
    entry.minted = true;
    entry.ticketed.clear();
    entry.removing.clear();
    if (!move.by) {
        ++counters_.moves_removal;
    } else if (move.step == MediaStepNeeded::Close) {
        ++counters_.moves_end;
    } else {
        ++counters_.moves_expel;
    }
    // The SFU hears of it only now that the write is done: the old generation closes once its
    // joins are answered, and the next ask opens the new one. Nobody else hears of the move
    // until the SFU says the old room is gone: a client that acts on call_moved (the one put out
    // trying its old credential among them) then finds nothing to join.
    retire(room, old, std::move(entry.media), std::exchange(entry.joining, 0), std::move(move));
    pump(room);
}

void CallHandler::remember_ticket(const core::RoomId& room, const core::UserId& user) noexcept {
    const auto it = rooms_.find(room);
    if (it == rooms_.end()) {
        return;
    }
    auto& ticketed = it->second.ticketed;
    const core::MonoTime now = clock_.now();
    std::erase_if(ticketed, [&](const auto& t) {
        return t.first == user || now - t.second >= kTicketLifetime;
    });
    try {
        if (ticketed.size() >= kMaxTicketed) {
            ticketed.erase(ticketed.begin());
        }
        ticketed.emplace_back(user, now);
    } catch (const std::bad_alloc&) {
        // Not remembered: an expulsion of this user asks the SFU instead.
        ++counters_.store_unavailable;
    }
}

bool CallHandler::expelled_from(const Entry& entry, const core::UserId& user) noexcept {
    return std::ranges::find(entry.expelled, user) != entry.expelled.end();
}

bool CallHandler::ticketed_lately(const Entry& entry, const core::UserId& user) const noexcept {
    const core::MonoTime now = clock_.now();
    return std::ranges::any_of(entry.ticketed, [&](const auto& t) {
        return t.first == user && now - t.second < kTicketLifetime;
    });
}

void CallHandler::announce(const core::RoomId& room, const Move& move) noexcept {
    if (move.call && move.step == MediaStepNeeded::Close) {
        ringer_.ended(room, *move.call, move.by);
    } else if (move.call && move.subject) {
        ringer_.moved(room, *move.call, move.by, *move.subject, true);
    }
}

void CallHandler::announce_retired(Retired& retired) noexcept {
    if (!retired.pending) {
        return;
    }
    const Move move = std::move(*retired.pending);
    retired.pending.reset();
    // Deposed since: the new owner knows nothing of the call, and what this one said could
    // contradict it.
    if (plane_.owner_generation(retired.room) != retired.owner_generation) {
        ++counters_.announcements_dropped;
        return;
    }
    announce(retired.room, move);
}

void CallHandler::fail_waiting(Entry& entry, CallOutcome outcome) noexcept {
    std::vector<Waiter> waiting = std::exchange(entry.waiting, {});
    for (Waiter& w : waiting) {
        --in_flight_;
        finish(w.answer, {.outcome = outcome, .ticket = std::nullopt});
    }
    // The moves stay, to be tried again; who asked for one is told to retry.
    for (Move& m : entry.moves) {
        if (m.answer) {
            finish(m.answer, {.outcome = CallOutcome::Unavailable, .ticket = std::nullopt});
            m.answer = nullptr;
        }
    }
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
                                   .attempt = 0,
                                   .attempt_deadline = now,
                                   .next_try = now,
                                   .give_up = now + limits_.close_retry_for,
                                   .owner_generation = plane_.owner_generation(room).value_or(0),
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
        for (Retired& r : retired_) {
            // A close whose answer never came (it was lost with an allocation that failed) is
            // tried again.
            if (r.closing && now >= r.attempt_deadline) {
                r.closing = false;
                r.next_try = now + backoff(r.attempt);
            }
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
        const std::uint32_t attempt = ++it->attempt;
        it->attempt_deadline = now + limits_.close_attempt;
        try {
            if (it->media) {
                it->media->close(
                    [this, id, attempt](std::expected<void, core::ports::MediaError> r) noexcept {
                        retired_closed(id, attempt, r);
                    });
                continue;
            }
            // Opened elsewhere: a handle is needed to close it, and opening is idempotent.
            sfu_->open_room(
                it->room, core::ports::MediaGeneration{it->generation},
                core::ports::MediaRoomKind::Call, 0,
                [this, id, attempt](
                    std::expected<std::unique_ptr<core::ports::IMediaRoom>, core::ports::MediaError>
                        opened) noexcept {
                    const auto r = std::ranges::find(retired_, id, &Retired::id);
                    if (r == retired_.end() || r->attempt != attempt) {
                        return;
                    }
                    if (!opened) {
                        retired_closed(id, attempt, std::unexpected(opened.error()));
                        return;
                    }
                    r->media = std::move(*opened);
                    r->media->close([this, id, attempt](
                                        std::expected<void, core::ports::MediaError> c) noexcept {
                        retired_closed(id, attempt, c);
                    });
                });
        } catch (const std::bad_alloc&) {
            it->closing = false;
            it->next_try = now + kRetryEvery;
        }
    }
}

void CallHandler::retired_closed(std::uint64_t id, std::uint32_t attempt,
                                 std::expected<void, core::ports::MediaError> r) noexcept {
    const auto it = std::ranges::find(retired_, id, &Retired::id);
    // An answer to an attempt given up on: the next one answers for it.
    if (it == retired_.end() || it->attempt != attempt) {
        return;
    }
    const bool retry = !r && (r.error() == core::ports::MediaError::Unavailable ||
                              r.error() == core::ports::MediaError::Remains);
    if (!retry) {
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
    it->next_try = clock_.now() + backoff(it->attempt);
    // The SFU could not be reached: the others are not kept waiting on it. A room it still lists
    // is another matter: the move is told only once it has gone.
    if (r.error() == core::ports::MediaError::Unavailable) {
        announce_retired(*it);
    }
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
    // Who is in the media room is asked first for a group call, whose cap the SFU applies only
    // once the client connects (so that the asker hears why); for a direct call that is on, whose
    // members may each bring two devices and no more (ADR-0131), and which may have nobody left
    // in it (ADR-0113); and for a callee answering a ring that should have its caller in it. A
    // direct call that still rings is answered at once, not held up by the SFU.
    if (entry.kind != CallKind::Group && !ringer_.caller_expected(room, waiter.request.user) &&
        !ringer_.on(room)) {
        issue(room, entry, std::move(waiter));
        return;
    }
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
    const auto call = ringer_.call_of(room);
    if (listed && call && ringer_.caller_expected(room, waiter.request.user)) {
        const auto caller = ringer_.caller_of(room);
        if (std::ranges::none_of(*listed, [&](const auto& p) { return p.user == caller; })) {
            // Answered, the callee would be alone: the caller is gone, and so is the ring.
            ringer_.caller_left(room, *call);
            ++counters_.no_call;
            --in_flight_;
            finish(waiter.answer, {.outcome = CallOutcome::NoCall, .ticket = std::nullopt});
            return;
        }
    }
    if (listed && call && listed->empty() && !waiter.request.answering && ringer_.settled(room)) {
        // Nobody is in the call any more, though nobody said so: it ends, and this ticket starts
        // the room's next call, which rings (ADR-0113).
        ringer_.abandoned(room, *call);
        restart(room, std::move(waiter));
        return;
    }
    // A direct call: each member on two devices at most, so the other member always fits
    // (ADR-0131). The asking device itself, connected already, is not another.
    if (listed && entry.kind == CallKind::Direct &&
        std::ranges::count_if(*listed, [&](const auto& p) {
            return p.user == waiter.request.user && p.device != waiter.request.device;
        }) >= kDirectDevicesPerMember) {
        ++counters_.full;
        --in_flight_;
        finish(waiter.answer, {.outcome = CallOutcome::Full, .ticket = std::nullopt});
        return;
    }
    if (listed && entry.kind == CallKind::Group) {
        const auto others = std::ranges::count_if(*listed, [&](const auto& p) {
            return p.user != waiter.request.user || p.device != waiter.request.device;
        });
        // People: each user once, the asker aside, who is in it already (another device of
        // theirs) or is the one more it would take (ADR-0115).
        std::vector<core::UserId> people;
        for (const core::ports::MediaParticipant& p : *listed) {
            if (p.user != waiter.request.user &&
                std::ranges::find(people, p.user) == people.end()) {
                people.push_back(p.user);
            }
        }
        const bool in_already = std::ranges::any_of(
            *listed, [&](const auto& p) { return p.user == waiter.request.user; });
        if (!in_already && people.size() >= people_cap(room, waiter)) {
            ++counters_.full;
            --in_flight_;
            finish(waiter.answer, {.outcome = CallOutcome::Full, .ticket = std::nullopt});
            return;
        }
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
    if (r == retired_.end() || r->joining == 0) {
        return;
    }
    --r->joining;
    if (r->joining == 0) {
        close_retired();
    }
}

void CallHandler::ticketed(const core::RoomId& room, const Waiter& waiter, rt::OwnerAnswer& answer,
                           core::ports::MediaTicket ticket) noexcept {
    // Rung once the ticket is in hand: a caller the SFU turned away rings nobody.
    const auto call = ringer_.ticketed(room, waiter.request.user, waiter.members, waiter.kind,
                                       waiter.request.ring);
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
    remember_ticket(room, waiter.request.user);
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
    case core::ports::MediaError::Remains:
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
    // One move per member and generation: the store may tell a removal more than once (a
    // notification, then a resync's check).
    if (entry != rooms_.end()) {
        auto& removing = entry->second.removing;
        if (std::ranges::find(removing, user) != removing.end()) {
            return;
        }
        // Marked before the SFU is asked: a duplicate arriving meanwhile is dropped too.
        try {
            if (removing.size() < kMaxTicketed) {
                removing.push_back(user);
            }
        } catch (const std::bad_alloc&) {
            // Not marked: a duplicate may move once more, which costs a reconnect.
            ++counters_.store_unavailable;
        }
    }
    if (!call) {
        // No call this node knows of; one it ticketed lately may still hold the member.
        if (entry != rooms_.end() && entry->second.media) {
            move_if_connected(room, entry->second.kind,
                              Move{.step = MediaStepNeeded::None,
                                   .call = std::nullopt,
                                   .by = std::nullopt,
                                   .subject = user,
                                   .answer = nullptr,
                                   .clear = false});
        }
        return;
    }
    const CallKind kind = ringer_.kind_of(room).value_or(CallKind::Direct);
    const MediaStepNeeded step = ringer_.removed(room, user);
    switch (step) {
    case MediaStepNeeded::None:
        move_if_connected(room, kind,
                          Move{.step = step,
                               .call = call,
                               .by = std::nullopt,
                               .subject = user,
                               .answer = nullptr,
                               .clear = false});
        return;
    case MediaStepNeeded::Move:
    case MediaStepNeeded::Close:
        enqueue(room, kind,
                Move{.step = step,
                     .call = call,
                     .by = std::nullopt,
                     .subject = user,
                     .answer = nullptr,
                     .clear = false});
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

void CallHandler::on_user_disconnected(const core::UserId& user) noexcept {
    if (sfu_ == nullptr) {
        return;
    }
    try {
        for (const core::RoomId& room : ringer_.rooms()) {
            if (plane_.owns(room) && ringer_.involves(room, user)) {
                ++counters_.disconnects;
                on_member_removed(room, user);
            }
        }
    } catch (const std::bad_alloc&) {
        // No list of the rooms: the user stays in their calls until they leave or are put out.
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
        if (it != rooms_.end() && it->second.busy) {
            // Its generation is being read or written: asked again at the next check, and not
            // counted as the SFU's silence.
            ringer_.occupied(room, call, std::nullopt, false);
            continue;
        }
        if (it != rooms_.end() && !it->second.media && it->second.minted &&
            it->second.ticketed.empty() && it->second.joining == 0) {
            // A generation this owner moved to and ticketed nobody into: nobody is in it.
            ++counters_.occupancy_checks;
            ringer_.occupied(room, call, false);
            continue;
        }
        if (it == rooms_.end() || !it->second.media) {
            // Nothing to ask with: someone may hold a ticket of an earlier owner's, which only
            // the SFU knows of; counted as the SFU's silence, so that the call ends in time.
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

std::size_t CallHandler::people_cap(const core::RoomId& room, const Waiter& waiter) const noexcept {
    std::size_t most = limits_.group_people;
    // The call's own, or the one this ticket would start.
    const std::uint16_t own =
        ringer_.idle(room) ? waiter.request.ring.max_people : ringer_.max_people(room);
    if (own != 0) {
        most = std::min<std::size_t>(most, std::max<std::uint16_t>(own, kMinGroupPeople));
    }
    return most;
}

void CallHandler::current(const core::RoomId& room, rt::OwnerAnswer answer) noexcept {
    try {
        auto snapshot = ringer_.snapshot(room);
        if (!snapshot) {
            finish(answer, {.outcome = CallOutcome::NoCall, .ticket = std::nullopt});
            return;
        }
        const auto it = rooms_.find(room);
        if (it == rooms_.end() || !it->second.media || it->second.busy) {
            // Nothing to ask the SFU with, or its generation is moving: who is in it is unknown.
            finish(answer, {.outcome = CallOutcome::Current,
                            .ticket = std::nullopt,
                            .snapshot = std::move(snapshot)});
            return;
        }
        // Counted until the SFU answers.
        ++in_flight_;
        try {
            it->second.media->participants(
                [this, snapshot = std::move(*snapshot), answer = std::move(answer)](
                    std::expected<std::vector<core::ports::MediaParticipant>,
                                  core::ports::MediaError>
                        listed) mutable noexcept {
                    --in_flight_;
                    try {
                        if (listed) {
                            std::vector<core::UserId> users;
                            for (const core::ports::MediaParticipant& p : *listed) {
                                if (std::ranges::find(users, p.user) == users.end()) {
                                    users.push_back(p.user);
                                }
                            }
                            snapshot.connected = std::move(users);
                        }
                        finish(answer, {.outcome = CallOutcome::Current,
                                        .ticket = std::nullopt,
                                        .snapshot = std::move(snapshot)});
                    } catch (const std::bad_alloc&) {
                        answer(std::unexpected(rt::RouteError::Unavailable));
                    }
                });
        } catch (const std::bad_alloc&) {
            // Whatever was moved into the callback answers nothing; the asker times out.
            --in_flight_;
            throw;
        }
    } catch (const std::bad_alloc&) {
        if (answer) {
            answer(std::unexpected(rt::RouteError::Unavailable));
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
        // A call's handle is kept while the call lasts: its occupancy is asked through it.
        const bool call = !ringer_.idle(entry.first);
        return !e.busy && e.waiting.empty() && e.moves.empty() && e.joining == 0 && !call &&
               now - e.used >= limits_.idle;
    });
}

} // namespace chat
