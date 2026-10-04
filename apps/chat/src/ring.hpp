#pragma once

#include "core/models/ids.hpp"
#include "core/ports/clock.hpp"
#include "core/ports/random.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <set>
#include <span>
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
// A group call whose occupancy the SFU could not tell this many checks in a row ends.
inline constexpr std::uint32_t kMaxUnansweredChecks = 3;

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
    // A group call nobody has asked a ticket of for answered_hold, then this often, asks the SFU
    // whether anyone is still connected: it ends (call_ended) when nobody is.
    core::Millis occupancy_check{30'000};
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
// cancels, or the ring timeout passes; then answered until a member ends it or answered_hold
// passes with no ticket asked. Everything runs on the reactor thread; time is the injected
// clock's, and deadlines are met by tick(), which the server runs after every turn of its loop.
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
    [[nodiscard]] std::expected<std::optional<CallId>, RingRefusal>
    ticketed(const core::RoomId& room, const core::UserId& user,
             const std::optional<std::vector<core::UserId>>& members,
             CallKind kind = CallKind::Direct) noexcept;
    // `user` was put out of the room's call and may not have a ticket for it while it lasts.
    [[nodiscard]] bool expelled(const core::RoomId& room, const core::UserId& user) const noexcept;
    // Whether a group call has room for one more member in it: kMaxGroupJoined. A member in it
    // already always fits.
    [[nodiscard]] bool fits(const core::RoomId& room, const core::UserId& user) const noexcept;

    // The caller of a group call ending it for everyone, or putting `target` out: whether the
    // step is allowed, and what it needs of the SFU. nullopt when the room has no such call, or
    // `user` is not its caller, or the target is the caller. An expulsion is remembered at once
    // (the target's tickets are refused); nothing is announced until the call handler's fenced
    // write is done and it calls ended() or moved().
    [[nodiscard]] std::optional<MediaStepNeeded>
    may_end(const core::RoomId& room, const core::UserId& user, const CallId& call) const noexcept;
    [[nodiscard]] std::optional<MediaStepNeeded> expel(const core::RoomId& room,
                                                       const core::UserId& user, const CallId& call,
                                                       const core::UserId& target) noexcept;
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
    // The id of the room's call, if it has one, and whose it is.
    [[nodiscard]] std::optional<CallId> call_of(const core::RoomId& room) const noexcept;
    [[nodiscard]] std::optional<CallKind> kind_of(const core::RoomId& room) const noexcept;
    // Users with a ticket in the room's call, not left: whom a resync of the member lists checks.
    [[nodiscard]] std::vector<core::UserId> joined(const core::RoomId& room) const;
    // Every room with a call.
    [[nodiscard]] std::vector<core::RoomId> rooms() const;

    // Group calls whose occupancy is due to be checked (RingLimits::occupancy_check), taken by
    // the call handler, which asks the SFU and answers occupied().
    [[nodiscard]] std::vector<std::pair<core::RoomId, CallId>> take_checks();
    // Whether anyone is connected to the call's media room: nobody ends it (call_ended), and
    // nullopt (the SFU did not answer) asks again at the next check.
    void occupied(const core::RoomId& room, const CallId& call,
                  std::optional<bool> anyone) noexcept;
    // A callee declines, the caller cancels, or a member of an answered call ends it. Answers
    // the call's caller when it did, nullopt when the room has no such call for this member to
    // end that way.
    [[nodiscard]] std::optional<core::UserId> signal(const core::RoomId& room,
                                                     const core::UserId& user, CallSignal signal,
                                                     const CallId& call) noexcept;
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
        // A group call's occupancy is being asked of the SFU, and how many checks in a row it
        // could not answer.
        bool checking = false;
        std::uint32_t unanswered = 0;
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
    void start(const core::RoomId& room, const core::UserId& caller,
               std::vector<core::UserId> callees, const CallId& id, CallKind kind);
    void schedule(const core::RoomId& room, Call& call, core::MonoTime due);
    // Schedules the call's next deadline from its state, or none while its occupancy is being
    // asked.
    void reschedule(const core::RoomId& room, Call& call);
    void tick_group(const core::RoomId& room, Calls::iterator it, core::MonoTime now) noexcept;
    // Takes `target` out of a group call, and remembers it put out; Move when it had a ticket.
    [[nodiscard]] MediaStepNeeded put_out(const core::RoomId& room, Call& call,
                                          const core::UserId& target);
    // Everyone a group call's events go to: its caller, callees and members in it.
    [[nodiscard]] static std::vector<core::UserId> members(const Call& call);
    void forget(Calls::iterator it) noexcept;
    // Tells every member of the call, the caller included, on every node they are connected to.
    void announce(const core::RoomId& room, const Call& call, RingEvent event,
                  const std::optional<core::UserId>& by) noexcept;
    // Tells one member.
    void tell(const core::RoomId& room, const Call& call, const core::UserId& to, RingEvent event,
              const std::optional<core::UserId>& by,
              const std::optional<core::UserId>& subject = std::nullopt) noexcept;
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
};

} // namespace chat
