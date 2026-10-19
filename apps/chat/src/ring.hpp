#pragma once

#include "core/models/ids.hpp"
#include "core/ports/clock.hpp"
#include "core/ports/random.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// A call's ring (ADR-0091): the signal that tells a direct chat's other member a call is
// starting, and tells both how it ended before anyone was talking. A group chat's call rings
// every other member, each until the ring timeout, and stays up while anyone is in it
// (ADR-0095). It lives on the room's
// owner, beside the call handler that issues the tickets (ADR-0087), and reaches each member's
// sockets wherever they are connected through the member's presence room, which every node with
// a socket of theirs is in (ADR-0056), as an unsequenced notice (rt::RoomRouter::notify).
namespace chat {

// Names one ring of one room, from the first ticket to its end: what the client matches the
// events of a call by.
using CallId = core::UuidId<struct CallTag>;

// What a notice says happened. The values are the layout's.
enum class RingEvent : std::uint8_t {
    // The caller asked for the call: it rings until expires_at.
    Ringing = 0,
    // A callee asked for a ticket: the call is on, and the callee's other devices stop ringing.
    Answered = 1,
    // A callee turned the call down.
    Declined = 2,
    // The caller gave up before anyone answered.
    Cancelled = 3,
    // Nobody answered within the ring timeout.
    Missed = 4,
    // A member ended the call after it was answered (a group call: its caller, for everyone;
    // or the last one in it left; or nobody was left in it).
    Ended = 5,
    // A group call's member left it; the call goes on for the others.
    Left = 6,
    // The call moved to a new generation of its media room, to put `subject` out (ADR-0050):
    // everyone else in it asks for a ticket again and connects with it.
    Moved = 7,
};

// Why a ring ended, when its event alone does not say (ADR-0113, ADR-0115). The values are the
// layout's.
enum class CallReason : std::uint8_t {
    // Cancelled: the caller's device was no longer in the call's media room while it rang, so
    // nobody would have been there to answer.
    CallerLeft = 1,
    // Declined: the callee is busy (another call), may not take calls now, or said no.
    Busy = 2,
    NotAllowed = 3,
    Declined = 4,
    // Cancelled: the room's owner changed (a deploy, a node lost) while the call rang, and the
    // new owner cannot carry the ring on (ADR-0129). Ring again to call.
    OwnerMoved = 5,
};
// How a reason reads on the wire to clients, the hook and the log: caller_left, busy, not_allowed,
// declined, moved.
[[nodiscard]] std::string_view reason_name(CallReason reason) noexcept;
// The reason a client names, if it is one a decline may give (busy, not_allowed, declined).
[[nodiscard]] std::optional<CallReason> decline_reason(std::string_view name) noexcept;

// A decline's quick reply ("In a meeting, call you at 6") is at most this many bytes of UTF-8.
inline constexpr std::size_t kMaxDeclineText = 200;

// A call's own cap on people (RingOptions::max_people) is within these: the caller and one other,
// and the most devices a group call ever takes (call.hpp, kMaxGroupParticipants).
inline constexpr std::uint16_t kMinCallPeople = 2;
inline constexpr std::uint16_t kMaxCallPeople = 16;

// How the first ticket of a call rings (ADR-0115): every other member, only those named (who
// must be on the room's member list), or nobody. A call that rings nobody is on at once, for
// whoever comes; it ends, as any call, when nobody is in it.
struct RingOptions {
    enum class Mode : std::uint8_t { All = 0, Only = 1, None = 2 };
    Mode mode = Mode::All;
    std::vector<core::UserId> only = {};
    // A group call's cap on people (each user once, whatever their devices); 0 is the operator's.
    std::uint16_t max_people = 0;

    friend bool operator==(const RingOptions&, const RingOptions&) = default;
};

// Who a room's call is between: the two members of a direct chat, or a group chat's.
enum class CallKind : std::uint8_t {
    Direct,
    Group,
};

// One notice for one member's sockets: what their nodes push to them.
struct CallNotice {
    RingEvent event = RingEvent::Ringing;
    core::UserId to;
    core::RoomId room;
    CallId call;
    // The caller.
    core::UserId from;
    // Who answered, declined, cancelled, ended, left or moved it; nobody for Ringing and Missed,
    // nor for an Ended or a Moved the owner decided itself (nobody left in the call, a member
    // removed from the chat).
    std::optional<core::UserId> by;
    // Ringing only: when it stops ringing if nobody answers. Unix milliseconds on the wire.
    core::WallTime expires_at;
    // Moved only: who was put out.
    std::optional<core::UserId> subject = std::nullopt;
    // Cancelled when nobody cancelled it, and Declined when the callee said why: why it ended.
    std::optional<CallReason> reason = std::nullopt;
    // Declined only: the callee's quick reply, at most kMaxDeclineText bytes, or empty.
    std::string text = {};
};

// The notices travel between nodes as opaque bytes; a decode that fails is a notice nobody
// hears.
[[nodiscard]] std::vector<std::byte> encode_notice(const CallNotice& notice);
[[nodiscard]] std::optional<CallNotice> decode_notice(std::span<const std::byte> bytes);

// What the ring needs of the room plane: to hand a notice to the nodes a user's sockets are on,
// and to know whether this node still owns a room (rt::RoomRouter's notify and owns).
class IRingPlane {
public:
    virtual ~IRingPlane() = default;
    virtual void notify(const core::RoomId& room, std::span<const std::byte> notice) noexcept = 0;
    [[nodiscard]] virtual bool owns(const core::RoomId& room) const noexcept = 0;
};

// A room's call as its owner knows it, for a status read (ADR-0114).
struct CallSnapshot {
    CallId call;
    CallKind kind = CallKind::Direct;
    core::UserId caller;
    // false: it still rings, nobody but the caller in it yet.
    bool answered = false;
    // Callees the call still rings.
    std::vector<core::UserId> ringing;
    // Who holds a ticket in it and has not left, the caller first.
    std::vector<core::UserId> joined;
    // Who the SFU lists in its media room, each once; nullopt when it could not say.
    std::optional<std::vector<core::UserId>> connected = std::nullopt;
    core::WallTime started_at;
    // When someone other than the caller first came.
    std::optional<core::WallTime> answered_at = std::nullopt;
    // When it stops ringing anyone still rung.
    core::WallTime expires_at;

    friend bool operator==(const CallSnapshot&, const CallSnapshot&) = default;
};

// What a ring's watcher hears (ADR-0111): each call's first Ringing and how its ring ends, once
// per event, not per member; not its re-announcements, nor a group call's Left and Moved.
struct CallEvent {
    RingEvent event = RingEvent::Ringing;
    core::RoomId room;
    CallId call;
    CallKind kind = CallKind::Direct;
    core::UserId caller;
    // Ringing: the members rung. Missed: who missed it (a group call that goes on: only those
    // still rung when it rang out). Otherwise empty.
    std::vector<core::UserId> users;
    // As CallNotice::by.
    std::optional<core::UserId> by;
    core::WallTime expires_at;
    // As CallNotice::reason and text.
    std::optional<CallReason> reason = std::nullopt;
    std::string text = {};
};

// Told of a ring's events on the room's owner, from inside the ringer: must not call back into it.
class IRingWatcher {
public:
    virtual ~IRingWatcher() = default;
    virtual void observed(const CallEvent& event) noexcept = 0;
};

// The most rings a room may be allowed per window: what each room's history has room for.
inline constexpr std::size_t kMaxRingsPerWindow = 16;
// A group call rings at most this many other members, the first of its member list in id order
// (a member it did not ring may still join). Every event is a notice to each of them, and a
// ringing call is announced again every 15 s.
inline constexpr std::size_t kMaxGroupCallees = 32;
// A group call remembers at most this many members in it (with a ticket, not left) and put
// out of it; a ticket past either is answered busy.
inline constexpr std::size_t kMaxGroupJoined = 64;
inline constexpr std::size_t kMaxExpelled = 32;
// A call whose occupancy the SFU could not tell this many checks in a row ends.
inline constexpr std::uint32_t kMaxUnansweredChecks = 3;
// A direct call's ring ends (call_cancelled, caller_left) once the SFU has listed nobody in its
// media room this many checks in a row (ADR-0113).
inline constexpr std::uint32_t kCallerAbsentChecks = 2;

struct RingLimits {
    // How long a call rings with nobody answering before both members are told it was missed.
    // What phones ring for before voicemail: 30 to 60 s. ULW_CALL_RING_TIMEOUT_MS overrides it.
    core::Millis ring_timeout{45'000};
    // A ringing call is announced again this often, to the sockets that connected since it
    // started and to any whose notice was lost on the way. Clients match by call id.
    core::Millis announce_every{15'000};
    // An answered call is remembered this long after its last ticket, so that a member asking
    // for a ticket again (an app restarted, a second device) joins it without ringing anyone. A
    // member who comes back later rings the other again (calls.md says what the client does).
    core::Millis answered_hold{120'000};
    // Calls ringing or answered at once on this node, as the owner of their rooms: about 300
    // bytes each, 1.2 MiB in all. A ticket that would start another is answered busy.
    std::size_t max_calls = 4'096;
    // Rings a room may start within ring_window: a ticket that would start one more is answered
    // ring_limited, with when to try again. A person calls, gives up, calls again a few times;
    // a loop of ticket and cancel would otherwise ring the other member every second or two.
    // 1 to kMaxRingsPerWindow; a value outside is taken as the nearest bound.
    std::uint32_t rings_per_window = 5;
    core::Millis ring_window{60'000};
    // After a callee declines, the caller may not ring the room again for this long (the
    // callee may call back at once): a decline is not answered by ringing again straight away.
    core::Millis decline_cooldown{30'000};
    // A callee's ticket asked before the ring timeout keeps the call ringing this much longer,
    // for the SFU to issue it: an answer just before expires_at is not lost to call_missed. The
    // SFU's open and join take up to 5 s each.
    core::Millis answer_grace{10'000};
    // A call nobody has asked a ticket of for answered_hold, then this often, asks the SFU
    // whether anyone is still connected: it ends (call_ended) when nobody is (ADR-0113).
    core::Millis occupancy_check{30'000};
    // A direct call's caller connects to the media room as soon as its ticket comes: from this
    // long after the ring starts, and then every caller_check, the owner asks the SFU whether
    // anyone is in it, and a ring nobody waits on ends (ADR-0113). A callee's ticket asked that
    // late is checked first too.
    core::Millis caller_grace{10'000};
    core::Millis caller_check{5'000};
    // Rooms whose recent rings are remembered for the two limits above: a History
    // (kMaxRingsPerWindow start times, the declined caller's id inline, and its deadline: under 300
    // bytes) plus its room id and the map's node and bucket, about 340 bytes each, 5.5 MiB in all.
    // Past it, a ring that would need another is answered busy.
    std::size_t max_histories = 16'384;
};

struct RingCounters {
    std::uint64_t started = 0;
    std::uint64_t answered = 0;
    std::uint64_t declined = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t missed = 0;
    std::uint64_t ended = 0;
    // Tickets refused because max_calls were ringing or answered, or max_histories remembered.
    std::uint64_t busy = 0;
    // Tickets refused because the room rang too often lately, or its caller was just declined.
    std::uint64_t limited = 0;
    // Rings kept past their timeout because a callee's ticket was being issued.
    std::uint64_t graced = 0;
    // Calls forgotten without a word because this node no longer owns their room: the members'
    // devices stop ringing at expires_at.
    std::uint64_t orphaned = 0;
    // Rings ended, call_cancelled with reason moved, because their room changed owner
    // (ADR-0129): by this node as it let the room go, or as it found a ring left behind.
    std::uint64_t moved = 0;
    // Direct calls' rings ended because their caller was no longer in the call.
    std::uint64_t caller_left = 0;
    // Calls found with nobody in them when a member asked for a ticket again: ended, and the
    // ticket rang a new call.
    std::uint64_t stale = 0;
    // Group calls a member left, and group calls ended because nobody was left in them.
    std::uint64_t left = 0;
    std::uint64_t emptied = 0;
    // Members put out of a call: by its caller, or removed from the chat.
    std::uint64_t expelled = 0;
    // Notices handed to the room plane, one per member per event.
    std::uint64_t notices = 0;
    std::uint64_t allocation_failures = 0;
};

// What a member asks of a call besides a ticket. The values are the ask layout's.
enum class CallSignal : std::uint8_t {
    Decline = 1,
    Cancel = 2,
    End = 3,
    // A group call only: leave it, while it goes on for the others.
    Leave = 4,
    // A group call only, its caller only: put a member out of the call.
    Expel = 5,
};

// What the call handler must do on the SFU for a step of a call: nothing, or move the call's
// media room to a new generation and close the old one (to put someone out), or close it with
// no successor (to end a group call for everyone). Done after the owner's fenced write only.
enum class MediaStepNeeded : std::uint8_t {
    None,
    Move,
    Close,
};

struct RingRefusal {
    enum class Why : std::uint8_t {
        // max_calls are ringing or answered, or max_histories rooms remembered.
        Busy,
        // The room rang rings_per_window times within ring_window, or the caller was declined
        // within decline_cooldown: try again after `retry_after`.
        Limited,
        // The user was put out of the room's call while it lasts.
        Expelled,
    };
    Why why = Why::Busy;
    core::Millis retry_after{0};
};

// The ring state machine of the rooms this node owns. A room has at most one call: ringing
// from its first ticket until a callee's ticket answers it, a callee declines, the caller
// cancels or leaves its media room, or the ring timeout passes; then on until a member ends it
// or the SFU lists nobody in it (ADR-0113). A group call nobody answered stays on while its
// caller is in it. Everything runs on the reactor thread; time is the injected clock's, and
// deadlines are met by tick(), which the server runs after every turn of its loop.
class Ringer {
public:
    Ringer(IRingPlane& plane, const core::ports::IClock& clock, core::ports::IRandom& random,
           RingLimits limits);

    // The room has no call: a ticket asked now may start one, and its member list is needed.
    [[nodiscard]] bool idle(const core::RoomId& room) const noexcept;
    // No room more can have a call.
    [[nodiscard]] bool full() const noexcept { return calls_.size() >= limits_.max_calls; }
    // How long until `caller` may start a ring in the room, when not now (the limits beside
    // RingLimits::rings_per_window); counted as a refusal.
    [[nodiscard]] std::optional<core::Millis> ring_limited(const core::RoomId& room,
                                                           const core::UserId& caller) noexcept;
    // A callee of the room's ringing call asked for a ticket: the call keeps ringing until the
    // ticket is issued, up to answer_grace past its timeout.
    void answering(const core::RoomId& room, const core::UserId& user) noexcept;

    // `user` was issued a ticket for the room's call. `members` is the room's member list when
    // the ask found the room idle, and nullopt otherwise. Answers the call the ticket belongs
    // to: the one ringing or answered (a callee's ticket answers a ringing call), or one this
    // ticket starts ringing for the other members. nullopt when there is none: nobody else is
    // on the list, or the ask found a call that has ended since.
    // `options` say how a call this ticket starts rings; a ticket joining a call ignores them.
    [[nodiscard]] std::expected<std::optional<CallId>, RingRefusal>
    ticketed(const core::RoomId& room, const core::UserId& user,
             const std::optional<std::vector<core::UserId>>& members,
             CallKind kind = CallKind::Direct, const RingOptions& options = {}) noexcept;
    // The room's call's own cap on people (RingOptions::max_people); 0 when it has none.
    [[nodiscard]] std::uint16_t max_people(const core::RoomId& room) const noexcept;
    // `user` was put out of the room's call and may not have a ticket for it while it lasts.
    [[nodiscard]] bool expelled(const core::RoomId& room, const core::UserId& user) const noexcept;
    // Whether a group call has room for one more member in it: kMaxGroupJoined. A member in it
    // already always fits.
    [[nodiscard]] bool fits(const core::RoomId& room, const core::UserId& user) const noexcept;

    // The caller of a group call, or an admin of the group (`admin`, ADR-0115), ending it for
    // everyone, or putting `target` out: whether the step is allowed, and what it needs of the
    // SFU. nullopt when the room has no such call, or `user` may not, or the target is `user`.
    // An expulsion is remembered at once (the target's tickets are refused); nothing is announced
    // until the call handler's fenced write is done and it calls ended() or moved().
    [[nodiscard]] std::optional<MediaStepNeeded> may_end(const core::RoomId& room,
                                                         const core::UserId& user,
                                                         const CallId& call,
                                                         bool admin = false) const noexcept;
    [[nodiscard]] std::optional<MediaStepNeeded> expel(const core::RoomId& room,
                                                       const core::UserId& user, const CallId& call,
                                                       const core::UserId& target,
                                                       bool admin = false) noexcept;
    // `user` is no longer on the room's member list: out of its call, as an expulsion by
    // nobody. What it needs of the SFU (Move when the user had a ticket in the call).
    [[nodiscard]] MediaStepNeeded removed(const core::RoomId& room,
                                          const core::UserId& user) noexcept;
    // After the fenced write: the call ended by `by` (call_ended to everyone), or moved to put
    // `subject` out (call_moved to everyone, `subject` included). Nothing when the room's call
    // is no longer `call`.
    void ended(const core::RoomId& room, const CallId& call,
               const std::optional<core::UserId>& by) noexcept;
    // `everyone` false: nothing moved (no device of theirs was in the call), and only `subject`
    // is told.
    void moved(const core::RoomId& room, const CallId& call, const std::optional<core::UserId>& by,
               const core::UserId& subject, bool everyone = true) noexcept;
    // Whether a ticket answering a ring may join the room's call: the room has one, and it is
    // not a direct call still ringing from `user` (CallRequest::answering).
    [[nodiscard]] bool answerable(const core::RoomId& room,
                                  const core::UserId& user) const noexcept;
    // Whether the room's call `call` still rings: for `user` when named (a group call's callee
    // still rung, or its caller while nobody has come), or at all. false: it is on, and does not
    // ring (for them). nullopt: the room has no such call, or `user` is not in it.
    [[nodiscard]] std::optional<bool>
    ringing(const core::RoomId& room, const CallId& call,
            const std::optional<core::UserId>& user) const noexcept;
    // Whether a callee's ticket for the room's ringing direct call should first find its caller
    // in the media room: the ring started caller_grace ago or more (ADR-0113).
    [[nodiscard]] bool caller_expected(const core::RoomId& room,
                                       const core::UserId& user) const noexcept;
    // The SFU lists nobody in the room's ringing direct call `call`: its ring ends, call_cancelled
    // with reason caller_left. Nothing when the room's call is no longer that, or no longer rings.
    void caller_left(const core::RoomId& room, const CallId& call) noexcept;
    // Whether the room's call is on (answered, or a group call whose ring is over) and its
    // latest ticket is caller_grace old: a ticket asked again then joins it, unless the SFU
    // lists nobody in it.
    [[nodiscard]] bool settled(const core::RoomId& room) const noexcept;
    // Whether the room's call is on: answered, or a group call whose ring is over.
    [[nodiscard]] bool on(const core::RoomId& room) const noexcept;
    // The caller of the room's call.
    [[nodiscard]] std::optional<core::UserId> caller_of(const core::RoomId& room) const noexcept;
    // The room's call, if it has one, as this owner knows it; `connected` is left for the caller
    // to fill from the SFU.
    [[nodiscard]] std::optional<CallSnapshot> snapshot(const core::RoomId& room) const;
    // The SFU lists nobody in the room's call `call`, which is on: it ends (call_ended, by
    // nobody), and the room has no call.
    void abandoned(const core::RoomId& room, const CallId& call) noexcept;
    // The id of the room's call, if it has one, and whose it is.
    [[nodiscard]] std::optional<CallId> call_of(const core::RoomId& room) const noexcept;
    [[nodiscard]] std::optional<CallKind> kind_of(const core::RoomId& room) const noexcept;
    // Users with a ticket in the room's call, not left: whom a resync of the member lists checks.
    [[nodiscard]] std::vector<core::UserId> joined(const core::RoomId& room) const;
    // Every room with a call.
    [[nodiscard]] std::vector<core::RoomId> rooms() const;
    // Whether `user` is in the room's call: its caller, rung by it, or with a ticket in it.
    [[nodiscard]] bool involves(const core::RoomId& room, const core::UserId& user) const noexcept;

    // Calls whose occupancy is due to be checked (RingLimits::occupancy_check, caller_check),
    // taken by the call handler, which asks the SFU and answers occupied().
    [[nodiscard]] std::vector<std::pair<core::RoomId, CallId>> take_checks();
    // Told of each group call that ends without a media generation move of its own (missed,
    // emptied, given up on), once the ringer has forgotten it: the owner then clears who was put
    // out of it (ADR-0095). Not told of a call the caller ended, nor of one a deposed owner drops.
    using GroupEnded = std::move_only_function<void(const core::RoomId&) noexcept>;
    void on_group_ended(GroupEnded hook) noexcept { group_ended_ = std::move(hook); }
    // Who hears each call's events (ADR-0111); nullptr: nobody, the default.
    void watch(IRingWatcher* watcher) noexcept { watcher_ = watcher; }

    // This node is letting its rooms go (a drain): every call that still rings nobody has
    // answered ends, call_cancelled with reason moved, rather than in silence (ADR-0129). Calls
    // that are on are left to their media rooms.
    void move_out() noexcept;
    // The room's previous owner rang this call and this node holds none: its members hear it
    // ended, call_cancelled with reason moved, as from the previous owner (ADR-0129).
    void end_moved(const core::RoomId& room, const CallId& call, CallKind kind,
                   const core::UserId& caller, std::vector<core::UserId> callees,
                   core::WallTime expires_at) noexcept;
    // Whether anyone is connected to the call's media room: nobody ends it (call_ended; a ringing
    // direct call's ring, call_cancelled caller_left, after kCallerAbsentChecks), and nullopt
    // (the SFU did not answer) asks again at the next check.
    // `asked` false: the SFU was not asked (nothing to ask with yet, or the media generation
    // is being written), which does not count toward kMaxUnansweredChecks.
    void occupied(const core::RoomId& room, const CallId& call, std::optional<bool> anyone,
                  bool asked = true) noexcept;
    // A callee declines (saying why, `reason` and `text`, when they did), the caller cancels, or
    // a member of an answered call ends it. Answers the call's caller when it did, nullopt when
    // the room has no such call for this member to end that way.
    [[nodiscard]] std::optional<core::UserId>
    signal(const core::RoomId& room, const core::UserId& user, CallSignal signal,
           const CallId& call, std::optional<CallReason> reason = std::nullopt,
           std::string_view text = {}) noexcept;
    // Rings out the calls whose timeout passed, announces again those due, and forgets answered
    // calls past their hold and calls of rooms this node no longer owns. Cheap when nothing is
    // due.
    void tick() noexcept;

    [[nodiscard]] std::size_t calls() const noexcept { return calls_.size(); }
    [[nodiscard]] const RingCounters& counters() const noexcept { return counters_; }

private:
    struct Call {
        CallId id;
        core::UserId caller;
        std::vector<core::UserId> callees;
        bool answered = false;
        CallKind kind = CallKind::Direct;
        // A group call's callees still ringing.
        std::vector<core::UserId> ringing = {};
        // A group call's members with a ticket in it and not left, the caller first.
        std::vector<core::UserId> joined = {};
        // Put out of the call while it lasts.
        std::vector<core::UserId> expelled = {};
        // Its occupancy is being asked of the SFU, and how many checks in a row it could not
        // answer.
        bool checking = false;
        std::uint32_t unanswered = 0;
        // A ringing direct call: when its caller is next looked for in the media room, and how
        // many checks in a row found nobody.
        core::MonoTime next_caller_check;
        std::uint32_t caller_absent = 0;
        core::MonoTime started;
        core::WallTime started_at;
        std::optional<core::WallTime> answered_at = std::nullopt;
        // RingOptions::max_people.
        std::uint16_t max_people = 0;
        // The latest ticket of the call: its device may be connecting still, which the SFU does
        // not list yet, so nobody listed within caller_grace of it ends nothing.
        core::MonoTime last_ticket;
        core::MonoTime ring_deadline;
        core::WallTime expires_at;
        core::MonoTime next_announce;
        core::MonoTime hold_until;
        // Until when a callee's ticket is being issued; rung out no earlier.
        core::MonoTime answering_until;
        // Its entry in due_.
        core::MonoTime due;
    };
    using Calls = std::unordered_map<core::RoomId, Call>;
    // A room's recent rings, for RingLimits::rings_per_window and decline_cooldown.
    struct History {
        // When its latest rings started: a ring buffer of the last `count` of them, at most
        // rings_per_window, the next to write at `next`.
        std::array<core::MonoTime, kMaxRingsPerWindow> starts{};
        std::uint8_t count = 0;
        std::uint8_t next = 0;
        std::optional<core::UserId> declined;
        core::MonoTime declined_until;
    };

    // Makes room for one more history, forgetting those no limit needs any more; false when
    // there is none.
    [[nodiscard]] bool remember(const core::RoomId& room);
    void prune(core::MonoTime now) noexcept;
    // `quiet`: it rings nobody, and is on at once.
    void start(const core::RoomId& room, const core::UserId& caller,
               std::vector<core::UserId> callees, const CallId& id, CallKind kind, bool quiet,
               std::uint16_t max_people);
    void schedule(const core::RoomId& room, Call& call, core::MonoTime due);
    // Schedules the call's next deadline from its state, or none while its occupancy is being
    // asked.
    void reschedule(const core::RoomId& room, Call& call);
    void tick_group(const core::RoomId& room, Calls::iterator it, core::MonoTime now) noexcept;
    void tick_direct(const core::RoomId& room, Calls::iterator it, core::MonoTime now) noexcept;
    // Takes `target` out of a group call, and remembers it put out; Move when it had a ticket.
    [[nodiscard]] MediaStepNeeded put_out(const core::RoomId& room, Call& call,
                                          const core::UserId& target);
    // Everyone a group call's events go to: its caller, callees and members in it.
    [[nodiscard]] static std::vector<core::UserId> members(const Call& call);
    void forget(Calls::iterator it) noexcept;
    // forget(), for a group call ending without a move: the hook is told after.
    void forget_ended(Calls::iterator it) noexcept;
    // Tells every member of the call, the caller included, on every node they are connected to.
    void announce(const core::RoomId& room, const Call& call, RingEvent event,
                  const std::optional<core::UserId>& by,
                  std::optional<CallReason> reason = std::nullopt,
                  std::string_view text = {}) noexcept;
    // The watcher, of an event of the call; `users` as CallEvent::users.
    void observe(const core::RoomId& room, const Call& call, RingEvent event,
                 const std::optional<core::UserId>& by, std::vector<core::UserId> users = {},
                 std::optional<CallReason> reason = std::nullopt,
                 std::string_view text = {}) noexcept;
    // Tells one member.
    void tell(const core::RoomId& room, const Call& call, const core::UserId& to, RingEvent event,
              const std::optional<core::UserId>& by,
              const std::optional<core::UserId>& subject = std::nullopt,
              std::optional<CallReason> reason = std::nullopt, std::string_view text = {}) noexcept;
    // Queues the call's occupancy check, unless one is being asked already.
    void check(const core::RoomId& room, Call& call) noexcept;
    // A ringing direct call's ring ends: its caller is not in the call.
    void ring_off(Calls::iterator it) noexcept;
    // A call that is on ends: nobody is in it.
    void empty(Calls::iterator it) noexcept;
    // A group call's ticket for `user`: answered or rejoined.
    void joined_group(const core::RoomId& room, Call& call, const core::UserId& user);

    [[nodiscard]] static bool member_of(const Call& call, const core::UserId& user) noexcept;

    IRingPlane& plane_;
    const core::ports::IClock& clock_;
    core::ports::IRandom& random_;
    RingLimits limits_;
    RingCounters counters_;
    Calls calls_;
    // When each call has something due: its ring timeout or next announcement while ringing,
    // the end of its hold once answered.
    std::set<std::pair<core::MonoTime, core::RoomId>> due_;
    // rings_per_window, within 1 to kMaxRingsPerWindow.
    std::size_t window_rings_;
    std::unordered_map<core::RoomId, History> histories_;
    core::MonoTime next_prune_;
    // Group calls due an occupancy check, until the call handler takes them.
    std::vector<std::pair<core::RoomId, CallId>> checks_;
    GroupEnded group_ended_;
    IRingWatcher* watcher_ = nullptr;
};

} // namespace chat
