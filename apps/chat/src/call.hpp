#pragma once

#include "core/models/ids.hpp"
#include "core/ports/clock.hpp"
#include "core/ports/media.hpp"
#include "core/ports/message_store.hpp"
#include "rt/room_router.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

// The call handler (ADR-0050): a member of a direct chat asks, over the room WebSocket, to join
// the room's call; the room's owner checks the member list, opens the room's media room on the
// SFU once, and answers with a ticket the client takes to the SFU itself. Offers, answers,
// candidates and TURN credentials never pass through here (ADR-0037).
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
    // The owner holds as many asks as it takes.
    Busy = 6,
};

struct CallRequest {
    core::UserId user;
    core::DeviceId device;
};

struct CallAnswer {
    CallOutcome outcome = CallOutcome::Unavailable;
    // With Ticket only.
    std::optional<core::ports::MediaTicket> ticket;
};

// The asks travel between nodes as opaque bytes (rt::RoomRouter::ask_owner); these are their
// layouts. A decode that fails is an answer nobody can act on.
[[nodiscard]] std::vector<std::byte> encode_request(const CallRequest& request);
[[nodiscard]] std::optional<CallRequest> decode_request(std::span<const std::byte> bytes);
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
};

// Answers call asks for the rooms this node owns. Everything runs on the reactor thread.
class CallHandler final : public rt::IOwnerService {
public:
    // `sfu` null: calls are not configured here, and every ask is answered Disabled. The store
    // and the SFU must outlive the handler, and the SFU must drop what it still owes the handler
    // without calling it (as the LiveKit adapter does when destroyed) once the handler is gone.
    CallHandler(core::ports::IMessageStore& messages, core::ports::ISfu* sfu,
                const core::ports::IClock& clock, CallLimits limits);
    ~CallHandler() override;
    CallHandler(const CallHandler&) = delete;
    CallHandler& operator=(const CallHandler&) = delete;
    CallHandler(CallHandler&&) = delete;
    CallHandler& operator=(CallHandler&&) = delete;

    void on_ask(const core::RoomId& room, std::span<const std::byte> request,
                rt::OwnerAnswer answer) noexcept override;
    // Lets go of the media rooms nobody asked for within CallLimits::idle. Cheap to call often:
    // it looks at the rooms once a second at most.
    void sweep() noexcept;

    [[nodiscard]] bool enabled() const noexcept { return sfu_ != nullptr; }
    [[nodiscard]] const CallCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] std::size_t rooms() const noexcept { return rooms_.size(); }

private:
    struct Waiter {
        CallRequest request;
        rt::OwnerAnswer answer;
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
    std::size_t in_flight_ = 0;
    core::MonoTime next_sweep_;
    // Declared last: the handles go before anything they were made with.
    std::unordered_map<core::RoomId, Entry> rooms_;
};

} // namespace chat
