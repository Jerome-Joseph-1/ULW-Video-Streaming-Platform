#pragma once

#include "core/models/ids.hpp"
#include "core/ports/ring_store.hpp"
#include "rt/room_router.hpp"

#include "chat_service.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace chat {

struct BellCounters {
    // Call events pushed to sockets here, one per socket.
    std::uint64_t pushed = 0;
    // Notices heard for a user with no socket here (in their presence grace, or watched from
    // here), which reach nobody.
    std::uint64_t unheard = 0;
    // Notices that did not decode, or whose room is not their member's presence room.
    std::uint64_t malformed = 0;
    std::uint64_t allocation_failures = 0;
    // Rings sent to a socket as it connected, from the store (ADR-0129), and reads that failed.
    std::uint64_t replayed = 0;
    std::uint64_t replay_failures = 0;
    // activity frames pushed to sockets here, one per socket (ADR-0137); a notice for a user
    // with no socket here is counted in unheard, one that does not decode in malformed.
    std::uint64_t activity = 0;
};

struct BellId {
    std::uint64_t value = 0;
    friend bool operator==(BellId, BellId) = default;
};

// Where a call's ring reaches the sockets of a member on this node (ADR-0091): the room plane
// hands it the notices sent to the presence rooms this node is in (rt::INoticeListener), and
// the bell pushes each, as its event, to every socket here of the member it names. A room's
// activity (ADR-0137) reaches them the same way, as an activity frame. Everything runs on the
// reactor thread.
class CallBell final : public rt::INoticeListener {
public:
    // With a ring store, the socket also hears at once every ring that is its user's now
    // (ADR-0129), as call_ringing, rather than at the next announcement.
    [[nodiscard]] BellId attach(IClient& client, const core::UserId& user);
    // Where the rooms' rings are read; nullptr: nowhere. Must outlive the bell.
    void replay_from(core::ports::IRingStore* store) noexcept { store_ = store; }
    // Who hears the notices of rooms that are not presence rooms (typing and read marks,
    // ADR-0138): the chat service; nullptr, the start, counts them as malformed.
    void hear_rooms(rt::INoticeListener* rooms) noexcept { rooms_ = rooms; }
    void detach(BellId id) noexcept;

    void on_notice(const core::RoomId& room, std::span<const std::byte> body) noexcept override;

    [[nodiscard]] const BellCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] std::size_t users() const noexcept { return users_.size(); }

private:
    struct Socket {
        std::uint64_t id;
        IClient* client;
    };

    void activity(const core::RoomId& room, std::span<const std::byte> body);
    void replay(const std::weak_ptr<int>& alive, std::uint64_t id, const core::UserId& user,
                core::ports::RingStoreResult<std::vector<core::ports::StoredRing>> rings) noexcept;

    std::uint64_t next_ = 1;
    BellCounters counters_;
    // A user's sockets here; each socket is in one list, and a user with none has no entry.
    std::unordered_map<core::UserId, std::vector<Socket>> users_;
    std::unordered_map<std::uint64_t, core::UserId> sockets_;
    // The event's text, kept between notices.
    std::string text_;
    core::ports::IRingStore* store_ = nullptr;
    rt::INoticeListener* rooms_ = nullptr;
    // The store's answers may come after the bell is gone.
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace chat
