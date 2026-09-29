#pragma once

#include "core/ports/auth.hpp"
#include "core/ports/clock.hpp"
#include "net/reactor.hpp"
#include "net/signals.hpp"
#include "net/slab.hpp"
#include "rt/room_router.hpp"

#include "chat_service.hpp"
#include "presence.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace chat {

class Session;

struct Limits {
    // Each connection may hold a 64 KiB message in its decoder, max_backlog of unsent output
    // and service.max_send_bytes_in_flight of sends: 64 + 256 + 128 = 448 KiB, and 1280 of them
    // 560 MiB at the very worst. The router adds its owner queues (64 MiB), node-channel
    // connections (32 x ~2.1 MiB) and recent message keys (10 MiB), and the chat service the
    // messages it keeps for resuming clients (32 MiB), presence its rooms (8 MiB): about
    // 740 MiB in all, inside a 1 GiB pod with room for the kernel's socket buffers (ADR-0036,
    // ADR-0043, ADR-0053). A connection that is only listening costs a few KiB.
    std::size_t max_connections = 1280;
    // Output a client has not read yet. A delivery is at most 64 KiB, so this is four of the
    // largest, or thousands of ordinary ones: a reader that far behind is closed, and resumes
    // from its last seq when it reconnects.
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
    ServiceLimits service;
    PresenceLimits presence;
};

// Who may open a socket: the cookie that carries the token, and the pages allowed to use it.
struct Access {
    std::string cookie;
    std::vector<std::string> allowed_origins;
};

struct Deps {
    core::NodeId node;
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

// The chat service's view of this node's RoomRouter.
class RouterRooms final : public IRooms {
public:
    explicit RouterRooms(rt::RoomRouter& router) noexcept : router_(router) {}

    void join(const core::RoomId& room, rt::IMember& member, rt::JoinCallback done) override {
        router_.join(room, member, std::move(done));
    }
    void leave(const core::RoomId& room, rt::IMember& member) noexcept override {
        router_.leave(room, member);
    }
    void send(const core::RoomId& room, rt::IMember& from, const core::UserId& sender,
              const rt::MessageKey& key, std::vector<std::byte> body,
              rt::SendCallback done) override {
        router_.send(room, from, sender, key, std::move(body), std::move(done));
    }

private:
    rt::RoomRouter& router_;
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
    [[nodiscard]] ChatService& chat() noexcept { return chat_; }
    [[nodiscard]] Presence& presence() noexcept { return presence_; }
    [[nodiscard]] Session* session(net::Slab<Session>::Handle handle) noexcept;
    void retire(net::Slab<Session>::Handle handle) noexcept;

private:
    Deps deps_;
    Access access_;
    Limits limits_;
    Counters counters_;
    RouterRooms rooms_;
    // Sessions detach from both as they close, so they outlive them.
    ChatService chat_;
    Presence presence_;
    net::Slab<Session> sessions_;
    bool draining_ = false;
    bool released_ = false;
    net::TimerId drain_timer_;
};

} // namespace chat
