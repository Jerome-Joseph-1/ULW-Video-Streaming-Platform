#pragma once

#include "core/ports/auth.hpp"
#include "core/ports/clock.hpp"
#include "net/reactor.hpp"
#include "net/signals.hpp"
#include "net/slab.hpp"
#include "rt/room_router.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace chat {

class Session;

struct Limits {
    // Each connection may hold a 64 KiB message in its decoder, max_backlog of unsent output
    // and max_send_bytes_in_flight of sends: 64 + 256 + 128 = 448 KiB, and 1280 of them 560 MiB
    // at the very worst. The router adds its owner queues (64 MiB) and node-channel
    // connections (32 x ~2.1 MiB), about 690 MiB in all, inside a 1 GiB pod with room for the
    // kernel's socket buffers (ADR-0034). A connection that is only listening costs a few KiB.
    std::size_t max_connections = 1280;
    // A client shows a handful of conversations at once; 64 bounds what one socket makes this
    // node track and subscribe to.
    std::size_t max_rooms_per_connection = 64;
    // Joins of new rooms per user, across all their connections: a join may create the room,
    // a row that outlives everyone in it. A fresh user may fill one connection's rooms at once;
    // after that one a second, far faster than a person opens conversations and far slower
    // than a script filling the table would like.
    std::uint32_t join_burst = 64;
    std::uint32_t joins_per_second = 1;
    // Bytes of a connection's sends not yet answered, each counted as its body plus 256 for
    // the rest of it. They sit in an owner's queue or on the node channel meanwhile, so they are
    // part of the connection's memory: two of the largest messages, or dozens of ordinary ones.
    std::size_t max_send_bytes_in_flight = std::size_t{128} * 1024;
    // Output a client has not read yet. A delivery is at most 64 KiB, so this is four of the
    // largest, or thousands of ordinary ones: a reader that far behind is closed, and resumes
    // from its last seq when it reconnects (M17).
    std::size_t max_backlog = std::size_t{256} * 1024;
    // ADR-0029: a read of tiny control frames decodes into thousands of Frames. A client sends
    // a Pong per Ping and perhaps a Ping of its own now and then; 8 in one read, or more than
    // a burst of 20 refilling at 10 a second, is a flood, and closes the connection with 1008.
    std::size_t max_control_per_read = 8;
    std::uint32_t control_burst = 20;
    std::uint32_t control_per_second = 10;
    // From accept to a complete upgrade request, like the gateway's header timeout.
    core::Millis handshake_timeout{10'000};
    // Idle connections are pinged at this interval, and closed once nothing at all has arrived
    // for idle_timeout: two missed Pongs and a margin. NAT and load balancer idle timers are
    // 60 s and more.
    core::Millis ping_interval{30'000};
    core::Millis idle_timeout{75'000};
    // Clients get a Close 1001 and this long to answer it before a drain cuts them off.
    core::Millis drain_deadline{5'000};
};

// Who may open a socket: the cookie that carries the token, and the pages allowed to use it.
struct Access {
    std::string cookie;
    std::vector<std::string> allowed_origins;
};

struct Deps {
    net::IReactor& reactor;
    rt::RoomRouter& router;
    core::ports::IJwtVerifier& verifier;
    const core::ports::IClock& clock;
};

struct Counters {
    std::uint64_t connections_accepted = 0;
    std::uint64_t connections_rejected = 0;
    std::uint64_t upgrades = 0;
    std::uint64_t auth_failures = 0;
    std::uint64_t origin_rejections = 0;
    std::uint64_t messages_received = 0;
    std::uint64_t messages_delivered = 0;
    std::uint64_t protocol_errors = 0;
    std::uint64_t control_floods = 0;
    std::uint64_t slow_consumers = 0;
    std::uint64_t allocation_failures = 0;
};

// What the room plane reports, written as log lines and kept as counters. Message bodies never
// reach it.
class RoomLog final : public rt::IRouterEvents {
public:
    explicit RoomLog(core::NodeId self) noexcept : self_(self) {}

    void on_fenced_out(const core::RoomId& room, std::uint64_t generation,
                       rt::OwnerWrite write) noexcept override;
    void on_took_room(const core::RoomId& room, std::uint64_t generation) noexcept override;
    void on_peer_lost(const core::NodeId& peer) noexcept override;
    void on_peer_refused(std::string_view why) noexcept override;
    void on_node_taken() noexcept override;

private:
    core::NodeId self_;
};

// chat_server's client side: WebSocket upgrades on /rt, authenticated with the gateway's
// verifiers, and the health endpoints. Everything runs on the reactor thread.
class ChatServer final : public net::IAcceptHandler,
                         public net::ISignalHandler,
                         public net::ITimerHandler {
public:
    ChatServer(Deps deps, Access access, Limits limits);
    ~ChatServer() override;
    ChatServer(const ChatServer&) = delete;
    ChatServer& operator=(const ChatServer&) = delete;
    ChatServer(ChatServer&&) = delete;
    ChatServer& operator=(ChatServer&&) = delete;

    void on_accept(os::UniqueFd conn) noexcept override;
    void on_signal(net::Signal signal) noexcept override;
    // The drain deadline.
    void on_timeout() noexcept override;

    // Stops accepting, sends every client Close 1001, and gives up this node's rooms so that
    // other nodes take them at once.
    void begin_drain() noexcept;
    // Destroys sessions and node connections the kernel has let go of. Call after each
    // run_once.
    void reap() noexcept;
    [[nodiscard]] bool finished() const noexcept {
        return draining_ && released_ && sessions_.size() == 0;
    }
    // /readyz: not draining, and the room plane reaches the database.
    [[nodiscard]] bool ready() const noexcept { return !draining_ && deps_.router.healthy(); }
    [[nodiscard]] std::size_t connections() const noexcept { return sessions_.size(); }
    [[nodiscard]] std::string render_metrics() const;

    [[nodiscard]] const Deps& deps() const noexcept { return deps_; }
    [[nodiscard]] const Access& access() const noexcept { return access_; }
    [[nodiscard]] const Limits& limits() const noexcept { return limits_; }
    [[nodiscard]] Counters& counters() noexcept { return counters_; }
    [[nodiscard]] Session* session(net::Slab<Session>::Handle handle) noexcept;
    // Takes one of the user's joins; false when they have none left.
    [[nodiscard]] bool admit_join(const core::UserId& user);
    void retire(net::Slab<Session>::Handle handle) noexcept;

private:
    Deps deps_;
    Access access_;
    Limits limits_;
    Counters counters_;
    net::Slab<Session> sessions_;
    struct JoinBucket {
        std::uint32_t tokens = 0;
        core::MonoTime refilled;
    };
    std::unordered_map<core::UserId, JoinBucket> joins_;
    bool draining_ = false;
    bool released_ = false;
    net::TimerId drain_timer_;
};

} // namespace chat
