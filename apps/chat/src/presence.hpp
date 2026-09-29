#pragma once

#include "core/models/ids.hpp"
#include "core/ports/clock.hpp"
#include "net/reactor.hpp"
#include "rt/room_router.hpp"

#include "chat_service.hpp"
#include "token_bucket.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace chat {

struct PresenceLimits {
    // A contact list or a conversation sidebar shows a few dozen people at once; 128 bounds what
    // one socket makes this node join and track.
    std::size_t max_watches_per_client = 128;
    // Watches that make this node start watching a user (the first here, each costing a hello
    // and later an unwatch, and perhaps a join that creates the room's row), per watching user
    // across their connections here. A fresh client may watch its whole list at once; after
    // that one a second, as for chat joins (ServiceLimits::join_burst). Watching and
    // unwatching in a loop is held to that too.
    std::uint32_t watch_burst = 128;
    std::uint32_t watches_per_second = 1;
    // Presence rooms this node is in at once: half the router's rt::RouterConfig::max_rooms
    // (16384), so that watching cannot crowd out chat rooms. A user connected here always gets
    // theirs, and those are at most Limits::max_connections (1280); watches past the cap are
    // answered busy. Each room costs about 1 KiB here, 8 MiB in all. The watch lists add 16
    // bytes per watch (a room pointer in the client's, an id in the room's): 1280 connections
    // of 128 watches are 2.6 MiB.
    std::size_t max_rooms = 8'192;
    // How long after a user's last connection here closes they still count as online. A page
    // reload, or a reconnect after a drain's Close 1001, is back within a second or two; a
    // client backing off 1, 2 and 4 s between tries is back within 7 s after three failures.
    // Ten seconds covers that, and a user who really left shows offline ten seconds later.
    core::Millis grace{10'000};
    // A node that announced a user online probes again this often while another node watches;
    // watching nodes forget an announcement, and the announcing node a watcher, not heard from
    // within `expiry`. That is how a node that died, or drained before a user's grace ran out,
    // stops holding them online, and a watching node that died stops the renewals. One renewal
    // may be lost to an owner changing hands (rt::kOwnerStaleAfter, 5 s) and a forward timing
    // out (3 s) and still land before the second is due; 2.5 renewals leave room for that and
    // a retry.
    core::Millis refresh{60'000};
    core::Millis expiry{150'000};
};

struct PresenceCounters {
    // Events this node put into presence rooms. A user nobody watches costs none.
    std::uint64_t sent = 0;
    std::uint64_t received = 0;
    // Presence changes pushed to watching clients.
    std::uint64_t notified = 0;
    // Announcements, and watching nodes, dropped because they stopped being renewed.
    std::uint64_t expired = 0;
    // Deliveries past a gap in a presence room's seqs, after which this node said again what
    // it had said there.
    std::uint64_t gaps = 0;
    std::uint64_t allocation_failures = 0;
};

struct PresenceClientId {
    std::uint64_t value = 0;
    friend bool operator==(PresenceClientId, PresenceClientId) = default;
};

// Who is online, for the clients that watch them (ADR-0056). Each user has a presence room,
// in the same registry and through the same owners and node channel as chat rooms. A node
// joins it while the user is connected there (to announce them) or while a client there
// watches them (to hear announcements). The room's order is the only coordination: every node
// in the room sees the same events in the same order, so every watching node reaches the same
// verdict once, with no owner-side logic.
//
// Events, each naming the node that sent it:
//   hello    a node started watching; nodes with the user connected answer `online`.
//   unwatch  a node stopped watching.
//   probe    the user is connected at the sender, which asks who watches: when it cannot know
//            (it joined a room something was said in) and, as a renewal, every minute; like
//            `online`, and watching nodes answer `ack`.
//   ack      a watching node answering a probe.
//   online   the user is connected at the sender: an answer to a hello.
//   offline  the user's grace at the sender ran out with no connection back.
// A user is online at a watching node while any node's announcement stands there. A node whose
// user has never been watched joins the room, finds its head at 0 (the room's last_seq: nothing
// was ever said in it), and says nothing: a user nobody watches costs no event. A node that
// sees a gap in the room's seqs says again what it had said there (a hello, an announcement),
// since whoever missed it with it cannot know.
//
// Everything runs on the reactor thread; time comes from the injected clock, deadlines from
// one timer on the reactor.
class Presence final : public net::ITimerHandler {
public:
    Presence(IRooms& rooms, net::IReactor& reactor, const core::ports::IClock& clock,
             const core::NodeId& self, PresenceLimits limits);
    // Leaves every room; the room plane must outlive it.
    ~Presence() override;
    Presence(const Presence&) = delete;
    Presence& operator=(const Presence&) = delete;
    Presence(Presence&&) = delete;
    Presence& operator=(Presence&&) = delete;

    // A connection of `user`, who is online from now until `grace` after their last one here
    // is detached.
    [[nodiscard]] PresenceClientId attach(IClient& client, const core::UserId& user);
    void detach(PresenceClientId id) noexcept;
    // Answers `watching` with what this node knows now; `presence` follows each change.
    void watch(PresenceClientId id, const core::UserId& user);
    void unwatch(PresenceClientId id, const core::UserId& user);

    void on_timeout() noexcept override;

    [[nodiscard]] const PresenceCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] std::size_t rooms() const noexcept { return rooms_.size(); }

private:
    struct Room;
    enum class Kind : std::uint8_t;

    struct Client {
        IClient* client;
        core::UserId user;
        // Rooms with this client in their `local`, which keeps them from being erased.
        std::vector<Room*> watching;
    };

    [[nodiscard]] static std::optional<Kind> kind_of(std::byte b) noexcept;
    [[nodiscard]] Room& room_of(const core::UserId& user);
    void delivered(Room& room, const rt::Message& message) noexcept;
    void joined(const core::UserId& user,
                std::expected<std::uint64_t, rt::RouteError> result) noexcept;
    void pump(Room& room, core::MonoTime now);
    void post(Room& room, Kind kind, core::MonoTime now);
    void posted(const core::UserId& user, Kind kind,
                std::expected<std::uint64_t, rt::RouteError> result) noexcept;
    void expire(Room& room, core::MonoTime now) noexcept;
    void show(Room& room) noexcept;
    void tell(IClient& client, std::string_view type, const Room& room) noexcept;
    [[nodiscard]] static bool idle(const Room& room) noexcept;
    void drop_watch(Room& room, PresenceClientId id) noexcept;
    void wake(Room& room) noexcept;
    void arm(core::MonoTime at) noexcept;

    IRooms& rooms_plane_;
    net::IReactor& reactor_;
    const core::ports::IClock& clock_;
    PresenceLimits limits_;
    // Names this run of this node in every event it sends: a node restarted under the same
    // name is a different sender, and whatever its last run announced runs out on its own.
    std::uint64_t tag_;
    std::uint64_t next_key_ = 1;
    std::uint64_t next_client_ = 1;
    PresenceCounters counters_;
    std::unordered_map<std::uint64_t, Client> clients_;
    std::unordered_map<core::UserId, std::unique_ptr<Room>> rooms_;
    std::unordered_map<core::UserId, TokenBucket> watch_joins_;
    // Rooms with something to do now, visited on the next timeout rather than inside the call
    // that found it: a delivery must not call back into the router (rt::IMember).
    std::vector<core::UserId> dirty_;
    net::TimerId timer_;
    std::optional<core::MonoTime> armed_at_;
    core::MonoTime next_scan_;
};

} // namespace chat
