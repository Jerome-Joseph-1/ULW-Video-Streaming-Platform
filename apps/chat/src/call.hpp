#pragma once

#include "core/models/ids.hpp"
#include "core/ports/clock.hpp"
#include "core/ports/media.hpp"
#include "core/ports/message_store.hpp"
#include "rt/room_router.hpp"

#include "ring.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <variant>
#include <vector>

// The call handler (ADR-0050): a member of a direct chat asks, over the room WebSocket, to join
// the room's call; the room's owner checks the member list, opens the room's media room on the
// SFU once, and answers with a ticket the client takes to the SFU itself. Offers, answers,
// candidates and TURN credentials never pass through here (ADR-0037). The first ticket rings the
// other member, and declining, cancelling and ending are asked of the owner the same way
// (ring.hpp, ADR-0091). A group chat has a call of up to CallLimits::group_participants devices,
// and putting a member out of a call, or ending a group call for everyone, moves the room's
// media generation by the owner's fenced write before the SFU hears of it (ADR-0050, ADR-0095).
namespace chat {

// A direct chat's call: two participants at most, each a device (ADR-0050).
inline constexpr std::uint16_t kCallParticipants = 2;
// A group chat's call: CallLimits::group_participants devices, within these bounds (ADR-0095).
inline constexpr std::uint16_t kMinGroupParticipants = 3;
inline constexpr std::uint16_t kMaxGroupParticipants = 16;
// The media generation a room starts at (migrations/0014). It moves on only to put someone out
// of the call or to end a group call, by the owner's fenced write (ADR-0050, ADR-0095).
inline constexpr core::ports::MediaGeneration kCallGeneration{1};

// What the owner answered, as the asking node reads it.
enum class CallOutcome : std::uint8_t {
    Ticket = 0,
    // The asker is not on the room's member list.
    NotMember = 1,
    // The room is neither a direct chat nor a group chat (ADR-0095): a stream's live chat, or a
    // room with no kind recorded.
    NotCallable = 2,
    // The SFU or the member list could not be reached; a retry may succeed.
    Unavailable = 3,
    // The SFU refused the request as made (its configuration, ours or its); retrying will not
    // help until an operator acts.
    Failed = 4,
    // The owner has no SFU configured.
    Disabled = 5,
    // The owner holds as many asks, or calls, as it takes.
    Busy = 6,
    // A signal for a call the room does not have, or not in a state this member may move it
    // from: ended already, answered (for a decline or a cancel), ringing (for an end), or the
    // caller's to cancel.
    NoCall = 7,
    // A signal did what it asked.
    Done = 8,
    // The ticket would start a ring the room may not start yet (RingLimits::rings_per_window,
    // decline_cooldown); retry_after says when it may.
    RingLimited = 9,
    // The asker was put out of the room's call, which still lasts.
    Expelled = 10,
    // The group call holds as many devices as it takes.
    Full = 11,
};

// A ticket for this device.
struct CallRequest {
    core::UserId user;
    core::DeviceId device;
};

// Decline, cancel, end, leave the room's call `call`, or put `target` out of it.
struct CallSignalRequest {
    core::UserId user;
    CallSignal signal = CallSignal::Decline;
    CallId call;
    // Expel only.
    std::optional<core::UserId> target = std::nullopt;
};

using CallAsk = std::variant<CallRequest, CallSignalRequest>;

struct CallAnswer {
    CallOutcome outcome = CallOutcome::Unavailable;
    // With Ticket only.
    std::optional<core::ports::MediaTicket> ticket;
    // With Ticket: the call the ticket belongs to, when it rings or was rung. With Done: the
    // call's id is the one asked about.
    std::optional<CallId> call = std::nullopt;
    // With Done: the call's caller, which the asker's own event names.
    std::optional<core::UserId> caller = std::nullopt;
    // With RingLimited: when to ask again.
    std::optional<core::Millis> retry_after = std::nullopt;
};

// The asks travel between nodes as opaque bytes (rt::RoomRouter::ask_owner); these are their
// layouts. A decode that fails is an answer nobody can act on.
[[nodiscard]] std::vector<std::byte> encode_request(const CallRequest& request);
[[nodiscard]] std::vector<std::byte> encode_request(const CallSignalRequest& request);
[[nodiscard]] std::optional<CallAsk> decode_request(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encode_answer(const CallAnswer& answer);
[[nodiscard]] std::optional<CallAnswer> decode_answer(std::span<const std::byte> bytes);

// What the call handler needs of the room plane beside the ring's: the owner's generation of a
// room, and the room's media generation, read or moved on by the owner's fenced write
// (rt::RoomRouter's owner_generation and media_generation).
class ICallPlane : public IRingPlane {
public:
    [[nodiscard]] virtual std::optional<std::uint64_t>
    owner_generation(const core::RoomId& room) const noexcept = 0;
    // May answer inside the call, when the answer is known at once.
    virtual void media_generation(const core::RoomId& room, const rt::MediaChange& change,
                                  rt::StoreCallback<std::optional<rt::MediaState>> done) = 0;
};

struct CallLimits {
    // Rooms whose media room this node keeps a handle for: a few hundred bytes each. A handle
    // unused for `idle` is let go; the next ask opens the room again, which costs the SFU one
    // idempotent create.
    std::size_t max_rooms = 16'384;
    core::Millis idle{300'000};
    // Asks being answered at once, node-wide: each holds a store read or an SFU call. Past it an
    // ask is answered Busy.
    std::size_t max_in_flight = 1'024;
    // The ring of the calls whose rooms this node owns.
    RingLimits ring = {};
    // Devices in a group chat's call: the SFU room's max_participants (ADR-0095, from M29's
    // capacity derivation). kMinGroupParticipants to kMaxGroupParticipants;
    // ULW_CALL_GROUP_PARTICIPANTS overrides it.
    std::uint16_t group_participants = 8;
    // Old media rooms waiting to be closed after a move, node-wide: an expulsion past it is
    // answered busy. A close the SFU could not answer is tried again every second, for
    // close_retry_for.
    std::size_t max_retired = 1'024;
    core::Millis close_retry_for{300'000};
    // A close that has not answered by then is taken as lost, and tried again.
    core::Millis close_attempt{30'000};
};

struct CallCounters {
    std::uint64_t tickets = 0;
    std::uint64_t not_member = 0;
    std::uint64_t not_callable = 0;
    // Media rooms opened (one per room and handle, however many asked at once).
    std::uint64_t opens = 0;
    // SFU calls that failed, by whether a retry may help.
    std::uint64_t sfu_unavailable = 0;
    std::uint64_t sfu_refused = 0;
    std::uint64_t store_unavailable = 0;
    std::uint64_t busy = 0;
    // Declines, cancels and ends asked of a call the room did not have in that state.
    std::uint64_t no_call = 0;
    // Tickets refused to a member put out of the call, and to a device past a group call's cap.
    std::uint64_t expelled = 0;
    std::uint64_t full = 0;
    // Media generations moved on, by why: to put someone out (expelled by the caller, or
    // removed from the chat), or to end the call.
    std::uint64_t moves_expel = 0;
    std::uint64_t moves_removal = 0;
    std::uint64_t moves_end = 0;
    // Moves the store fenced (the room changed hands) or did not answer.
    std::uint64_t moves_fenced = 0;
    std::uint64_t moves_unavailable = 0;
    // Old media rooms closed after a move, and given up on after close_retry_for.
    std::uint64_t retired_closed = 0;
    std::uint64_t retired_abandoned = 0;
    // Members put out of a generation that did not move (no device of theirs in it), and moves
    // a deposed owner did not tell of.
    std::uint64_t expulsions_kept = 0;
    std::uint64_t announcements_dropped = 0;
    // Group calls' occupancy asked of the SFU, and the answers it could not give.
    std::uint64_t occupancy_checks = 0;
    std::uint64_t occupancy_unavailable = 0;
    // Member lists checked again after the store lost its way to hear removals.
    std::uint64_t resync_checks = 0;
};

// Answers call asks for the rooms this node owns. Everything runs on the reactor thread.
class CallHandler final : public rt::IOwnerService, public core::ports::IMemberListener {
public:
    // `sfu` null: calls are not configured here, and every ask is answered Disabled. The store,
    // the SFU and the plane must outlive the handler, and the SFU must drop what it still owes
    // the handler without calling it (as the LiveKit adapter does when destroyed) once the
    // handler is gone. `plane` carries the ring's notices and the media generation.
    CallHandler(core::ports::IMessageStore& messages, core::ports::ISfu* sfu, ICallPlane& plane,
                const core::ports::IClock& clock, core::ports::IRandom& random, CallLimits limits);
    ~CallHandler() override;
    CallHandler(const CallHandler&) = delete;
    CallHandler& operator=(const CallHandler&) = delete;
    CallHandler(CallHandler&&) = delete;
    CallHandler& operator=(CallHandler&&) = delete;

    void on_ask(const core::RoomId& room, std::span<const std::byte> request,
                rt::OwnerAnswer answer) noexcept override;
    // A member removed from a room whose call this node owns is put out of it (ADR-0050): the
    // media generation moves on, and the old one closes under them.
    void on_member_removed(const core::RoomId& room, const core::UserId& user) noexcept override;
    // Removals may have gone unheard: the members in every call here are checked again.
    void on_members_resync() noexcept override;
    // Someone listed, or a role changed (ADR-0096), puts nobody out of a call: a new member asks
    // for a ticket like any other, and who may expel is the call's caller, not the group's admin.
    void on_member_added(const core::RoomId& /*room*/,
                         const core::UserId& /*user*/) noexcept override {}
    void on_member_role(const core::RoomId& /*room*/, const core::UserId& /*user*/,
                        core::ports::MemberRole /*role*/) noexcept override {}
    // Lets go of the media rooms nobody asked for within CallLimits::idle, and runs the ring's
    // deadlines. Cheap to call often: it looks at the rooms once a second at most, and at the
    // ring's next deadline only.
    void sweep() noexcept;

    [[nodiscard]] bool enabled() const noexcept { return sfu_ != nullptr; }
    [[nodiscard]] const CallCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] const RingCounters& ring_counters() const noexcept { return ringer_.counters(); }
    [[nodiscard]] std::size_t rooms() const noexcept { return rooms_.size(); }
    // Calls ringing or answered in the rooms this node owns.
    [[nodiscard]] std::size_t calls() const noexcept { return ringer_.calls(); }
    // Old media rooms not closed yet.
    [[nodiscard]] std::size_t retired() const noexcept { return retired_.size(); }

private:
    struct Waiter {
        CallRequest request;
        rt::OwnerAnswer answer;
        // The room's member list, read when the ask found the room with no call: the ticket
        // may start one, which rings the others.
        std::optional<std::vector<core::UserId>> members = std::nullopt;
        CallKind kind = CallKind::Direct;
    };
    // A media generation to move on from: to put `subject` out (Move), or to end a group call
    // (Close); or, None, `subject` put out of the generation as it stands (no device of theirs
    // in it). `answer` is the asker's, empty for a removal, which is retried until done.
    struct Move {
        MediaStepNeeded step = MediaStepNeeded::Move;
        // None for a member removed while this node knows no call in the room.
        std::optional<CallId> call;
        std::optional<core::UserId> by;
        std::optional<core::UserId> subject;
        rt::OwnerAnswer answer;
    };
    // A room's media room on the SFU, opened once per generation and reused for every ask. While
    // its generation is read, its media room opened or its generation moved (`busy`), asks and
    // moves wait in order.
    struct Entry {
        CallKind kind = CallKind::Direct;
        // The media generation, as read or written under the owner's `owner_generation`; 0
        // before it is read.
        std::uint64_t generation = 0;
        std::uint64_t owner_generation = 0;
        // The generation is to be read again before anything else: a move's outcome is unknown.
        bool reread = false;
        std::unique_ptr<core::ports::IMediaRoom> media;
        bool busy = false;
        std::vector<Waiter> waiting;
        std::deque<Move> moves;
        // A removal's move the store could not take is tried again no sooner than this.
        core::MonoTime stalled_until;
        // Who is put out of `generation`, as the store keeps it with the generation.
        std::vector<core::UserId> expelled;
        // Tickets issued in `generation` lately, by user: a device of theirs may be connected
        // without the SFU listing it yet. Cleared with each move.
        std::vector<std::pair<core::UserId, core::MonoTime>> ticketed;
        // Members whose removal is queued for `generation`: one move each.
        std::vector<core::UserId> removing;
        // Joins asked of `media` and not answered yet: the entry is kept while there are any.
        std::size_t joining = 0;
        core::MonoTime used;
    };
    // A generation moved away from, closed once the joins asked of it are answered, and tried
    // again until the SFU says it is closed. `media` null: this node never opened it (another
    // owner did), so it is opened to be closed.
    struct Retired {
        std::uint64_t id = 0;
        core::RoomId room;
        std::uint64_t generation = 0;
        std::unique_ptr<core::ports::IMediaRoom> media;
        std::size_t joining = 0;
        bool closing = false;
        // The close in flight, and when it is taken as lost: its answer may never come.
        std::uint32_t attempt = 0;
        core::MonoTime attempt_deadline;
        core::MonoTime next_try;
        core::MonoTime give_up;
        // The owner generation it was retired under: a deposed owner says nothing of it.
        std::uint64_t owner_generation = 0;
        // The move that retired it, told to everyone once the SFU says the room is gone, or
        // could not be reached the first time (the asker was answered at the write).
        std::optional<Move> pending;
    };

    void checked(const core::RoomId& room, Waiter waiter,
                 core::ports::MessageResult<core::ports::RoomAccess> access) noexcept;
    // Whether the room is a direct or group chat the user is on; answers the ask when not, or
    // when the access could not be read.
    [[nodiscard]] bool callable(const core::ports::MessageResult<core::ports::RoomAccess>& access,
                                rt::OwnerAnswer& answer) noexcept;
    // The group call's caller ending it for everyone, or putting someone out.
    void moderate(const core::RoomId& room, const CallSignalRequest& request,
                  rt::OwnerAnswer& answer) noexcept;
    void moderated(const core::RoomId& room, const CallSignalRequest& request,
                   rt::OwnerAnswer& answer) noexcept;
    // Moves the media generation on to put `move.subject` out when a device of theirs may be in
    // it (a ticket of theirs lately, or the SFU lists one or cannot say); otherwise an expulsion
    // is kept with the generation as it stands, and a removal needs nothing.
    void move_if_connected(const core::RoomId& room, CallKind kind, Move move) noexcept;
    // Queues a move and runs the room's queue.
    void enqueue(const core::RoomId& room, CallKind kind, Move move) noexcept;
    // Runs what the room's entry has waiting, in order: its generation read (again, after a
    // change of owner), a move, its media room's open, then the asks.
    void pump(const core::RoomId& room) noexcept;
    void read(const core::RoomId& room,
              rt::StoreResult<std::optional<rt::MediaState>> result) noexcept;
    void advanced(const core::RoomId& room,
                  rt::StoreResult<std::optional<rt::MediaState>> result) noexcept;
    // Whether `user` was put out of the generation the entry holds, or had a ticket of it lately.
    [[nodiscard]] static bool expelled_from(const Entry& entry, const core::UserId& user) noexcept;
    void remember_ticket(const core::RoomId& room, const core::UserId& user) noexcept;
    [[nodiscard]] bool ticketed_lately(const Entry& entry, const core::UserId& user) const noexcept;
    // This node does not own the room (any more): everything waiting is answered Unavailable,
    // and the entry dropped without closing anything; the new owner holds the generation.
    void drop(const core::RoomId& room) noexcept;
    // Answers what waits on the entry as failed: asks with `outcome`, moves with Unavailable;
    // a removal's move stays, to be tried again.
    void fail_waiting(Entry& entry, CallOutcome outcome) noexcept;
    void retire(const core::RoomId& room, std::uint64_t generation,
                std::unique_ptr<core::ports::IMediaRoom> media, std::size_t joining,
                Move move) noexcept;
    // Tells everyone of a move.
    void announce(const core::RoomId& room, const Move& move) noexcept;
    void announce_retired(Retired& retired) noexcept;
    void close_retired() noexcept;
    void retired_closed(std::uint64_t id, std::uint32_t attempt,
                        std::expected<void, core::ports::MediaError> r) noexcept;
    // A join asked of `generation` was answered.
    void joined(const core::RoomId& room, std::uint64_t generation) noexcept;
    void counted(const core::RoomId& room, std::uint64_t generation, Waiter waiter,
                 std::expected<std::vector<core::ports::MediaParticipant>, core::ports::MediaError>
                     listed) noexcept;
    void issue(const core::RoomId& room, Entry& entry, Waiter waiter) noexcept;
    void check_occupancy() noexcept;
    [[nodiscard]] std::uint16_t cap(CallKind kind) const noexcept {
        return kind == CallKind::Group ? limits_.group_participants : kCallParticipants;
    }
    void listed(const core::RoomId& room, Waiter waiter,
                core::ports::MessageResult<std::vector<core::UserId>> members) noexcept;
    void signal(const core::RoomId& room, const CallSignalRequest& request, rt::OwnerAnswer answer);
    void signalled(const core::RoomId& room, const CallSignalRequest& request,
                   rt::OwnerAnswer& answer,
                   const core::ports::MessageResult<core::ports::RoomAccess>& access) noexcept;
    void ticketed(const core::RoomId& room, const Waiter& waiter, rt::OwnerAnswer& answer,
                  core::ports::MediaTicket ticket) noexcept;
    void admit(const core::RoomId& room, Waiter waiter) noexcept;
    void opened(const core::RoomId& room, std::uint64_t generation,
                std::expected<std::unique_ptr<core::ports::IMediaRoom>, core::ports::MediaError>
                    result) noexcept;
    void join(const core::RoomId& room, Entry& entry, Waiter waiter) noexcept;
    // Answers the asks waiting for the room's open Unavailable, and forgets the room unless it
    // holds a handle, joins or moves: the open could not be asked.
    void abandon_open(const core::RoomId& room) noexcept;
    // sweep(), now, whenever it last ran: for a new room at the cap.
    void sweep_now() noexcept;
    static void finish(rt::OwnerAnswer& answer, const CallAnswer& outcome) noexcept;
    [[nodiscard]] CallOutcome failure(core::ports::MediaError error) noexcept;

    core::ports::IMessageStore& messages_;
    core::ports::ISfu* sfu_;
    ICallPlane& plane_;
    const core::ports::IClock& clock_;
    CallLimits limits_;
    CallCounters counters_;
    Ringer ringer_;
    std::size_t in_flight_ = 0;
    core::MonoTime next_sweep_;
    std::uint64_t next_retired_ = 0;
    // The plane's callbacks may outlive the handler (a store call in flight): they hold this
    // weakly and do nothing once it is gone.
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    // Declared last: the handles go before anything they were made with.
    std::vector<Retired> retired_;
    std::unordered_map<core::RoomId, Entry> rooms_;
};

} // namespace chat
