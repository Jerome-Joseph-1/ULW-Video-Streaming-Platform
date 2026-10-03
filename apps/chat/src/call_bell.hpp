#pragma once

#include "core/models/ids.hpp"
#include "rt/room_router.hpp"

#include "chat_service.hpp"

#include <cstddef>
#include <cstdint>
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
};

struct BellId {
    std::uint64_t value = 0;
    friend bool operator==(BellId, BellId) = default;
};

// Where a call's ring reaches the sockets of a member on this node (ADR-0092): the room plane
// hands it the notices sent to the presence rooms this node is in (rt::INoticeListener), and
// the bell pushes each, as its event, to every socket here of the member it names. Everything
// runs on the reactor thread.
class CallBell final : public rt::INoticeListener {
public:
    [[nodiscard]] BellId attach(IClient& client, const core::UserId& user);
    void detach(BellId id) noexcept;

    void on_notice(const core::RoomId& room, std::span<const std::byte> body) noexcept override;

    [[nodiscard]] const BellCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] std::size_t users() const noexcept { return users_.size(); }

private:
    struct Socket {
        std::uint64_t id;
        IClient* client;
    };

    std::uint64_t next_ = 1;
    BellCounters counters_;
    // A user's sockets here; each socket is in one list, and a user with none has no entry.
    std::unordered_map<core::UserId, std::vector<Socket>> users_;
    std::unordered_map<std::uint64_t, core::UserId> sockets_;
    // The event's text, kept between notices.
    std::string text_;
};

} // namespace chat
