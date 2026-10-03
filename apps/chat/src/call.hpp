#pragma once

#include "core/models/ids.hpp"
#include "core/ports/clock.hpp"
#include "core/ports/media.hpp"
#include "core/ports/message_store.hpp"
#include "rt/room_router.hpp"

#include "ring.hpp"

#include <cstddef>
#include <cstdint>
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
// (ring.hpp, ADR-0091).
namespace chat {

// A call is a direct chat's: two participants at most, each a device (ADR-0050). Group calls
// are not offered (ADR-0058).
inline constexpr std::uint16_t kCallParticipants = 2;
// The generation a room's call runs in. A generation moves on only to put someone out, by the
// owner's fenced write of the room's state (ADR-0050); this handler does not put anyone out
// yet (ADR-0087), so every call stays in its first, and any owner opens the same media room for
// it.
inline constexpr core::ports::MediaGeneration kCallGeneration{1};

// What the owner answered, as the asking node reads it.
enum class CallOutcome : std::uint8_t {
    Ticket = 0,
    // The asker is not on the room's member list.
    NotMember = 1,
    // The room is not a direct chat: a group chat (group calls are not offered, ADR-0058), a
    // stream's live chat, or a room with no kind recorded.
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
};

// A ticket for this device.
struct CallRequest {
    core::UserId user;
    core::DeviceId device;
};

// Decline, cancel or end the room's call `call`.
struct CallSignalRequest {
    core::UserId user;
    CallSignal signal = CallSignal::Decline;
    CallId call;
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
};

// Answers call asks for the rooms this node owns. Everything runs on the reactor thread.
class CallHandler final : public rt::IOwnerService {
public:
    // `sfu` null: calls are not configured here, and every ask is answered Disabled. The store,
    // the SFU and the plane must outlive the handler, and the SFU must drop what it still owes
    // the handler without calling it (as the LiveKit adapter does when destroyed) once the
    // handler is gone. `plane` carries the ring's notices; `push`, when given, hears each ring
    // that starts (Web Push, ADR-0097) and must outlive the handler.
    CallHandler(core::ports::IMessageStore& messages, core::ports::ISfu* sfu, IRingPlane& plane,
                const core::ports::IClock& clock, core::ports::IRandom& random, CallLimits limits,
                IRingPush* push = nullptr);
    ~CallHandler() override;
    CallHandler(const CallHandler&) = delete;
    CallHandler& operator=(const CallHandler&) = delete;
    CallHandler(CallHandler&&) = delete;
    CallHandler& operator=(CallHandler&&) = delete;

    void on_ask(const core::RoomId& room, std::span<const std::byte> request,
                rt::OwnerAnswer answer) noexcept override;
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

private:
    struct Waiter {
        CallRequest request;
        rt::OwnerAnswer answer;
        // The room's member list, read when the ask found the room with no call: the ticket
        // may start one, which rings the others.
        std::optional<std::vector<core::UserId>> members = std::nullopt;
    };
    // A room's media room on the SFU, opened once and reused for every ask; the asks that came
    // while it was opening wait for it.
    struct Entry {
        std::unique_ptr<core::ports::IMediaRoom> media;
        std::vector<Waiter> opening;
        // Joins asked of `media` and not answered yet: the entry is kept while there are any.
        std::size_t joining = 0;
        core::MonoTime used;
    };

    void checked(const core::RoomId& room, Waiter waiter,
                 core::ports::MessageResult<core::ports::RoomAccess> access) noexcept;
    // Whether the room is a direct chat the user is on; answers the ask when not, or when the
    // access could not be read.
    [[nodiscard]] bool callable(const core::ports::MessageResult<core::ports::RoomAccess>& access,
                                rt::OwnerAnswer& answer) noexcept;
    void listed(const core::RoomId& room, Waiter waiter,
                core::ports::MessageResult<std::vector<core::UserId>> members) noexcept;
    void signal(const core::RoomId& room, const CallSignalRequest& request, rt::OwnerAnswer answer);
    void signalled(const core::RoomId& room, const CallSignalRequest& request,
                   rt::OwnerAnswer& answer,
                   const core::ports::MessageResult<core::ports::RoomAccess>& access) noexcept;
    void ticketed(const core::RoomId& room, const Waiter& waiter, rt::OwnerAnswer& answer,
                  core::ports::MediaTicket ticket) noexcept;
    void admit(const core::RoomId& room, Waiter waiter) noexcept;
    void opened(const core::RoomId& room,
                std::expected<std::unique_ptr<core::ports::IMediaRoom>, core::ports::MediaError>
                    result) noexcept;
    void join(const core::RoomId& room, Entry& entry, Waiter waiter) noexcept;
    // Answers the asks waiting for the room's open Unavailable, and forgets the room unless it
    // holds a handle or joins: the open could not be asked.
    void abandon_open(const core::RoomId& room) noexcept;
    // sweep(), now, whenever it last ran: for a new room at the cap.
    void sweep_now() noexcept;
    static void finish(rt::OwnerAnswer& answer, const CallAnswer& outcome) noexcept;
    [[nodiscard]] CallOutcome failure(core::ports::MediaError error) noexcept;

    core::ports::IMessageStore& messages_;
    core::ports::ISfu* sfu_;
    const core::ports::IClock& clock_;
    CallLimits limits_;
    CallCounters counters_;
    Ringer ringer_;
    std::size_t in_flight_ = 0;
    core::MonoTime next_sweep_;
    // Declared last: the handles go before anything they were made with.
    std::unordered_map<core::RoomId, Entry> rooms_;
};

} // namespace chat
