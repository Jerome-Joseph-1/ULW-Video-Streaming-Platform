#pragma once

#include "core/ports/auth.hpp"
#include "core/ports/clock.hpp"
#include "core/ports/random.hpp"
#include "http/client_limits.hpp"
#include "net/ip_address.hpp"
#include "net/reactor.hpp"
#include "net/signals.hpp"
#include "net/slab.hpp"
#include "rt/room_router.hpp"

#include "chat_service.hpp"
#include "presence.hpp"
#include "token_bucket.hpp"

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
    // messages it keeps for resuming clients (32 MiB), presence its rooms and watch lists (11 MiB):
    // about 740 MiB in all. The kernel's send buffers add 80 MiB (socket_send_buffer), 820 MiB
    // inside a 1 GiB pod (ADR-0036, ADR-0043, ADR-0056, ADR-0070). A connection that is only
    // listening costs a few KiB.
    std::size_t max_connections = 1280;
    // Output a client has not read yet. A delivery is at most 64 KiB, so this is four of the
    // largest, or thousands of ordinary ones: a reader that far behind is closed, and resumes
    // from its last seq when it reconnects.
    std::size_t max_backlog = std::size_t{256} * 1024;
    // The kernel's send buffer of each client connection, which Linux doubles to 64 KiB.
    // Autotuned, it grows to tcp_wmem's 4 MiB for a peer that stops reading: 5 GiB over 1280
    // connections, charged to the pod although outside the process. Fixed, they hold 80 MiB at
    // most, and a lossy client's lag is set by the service's limits, not by the kernel's
    // megabytes ahead of them (ADR-0070). 64 KiB a round trip is 640 KB/s at 100 ms, a
    // history page in four round trips.
    int socket_send_buffer = 32 * 1024;
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
    // A client whose connection has output waiting for it, and which acknowledges none of it for
    // stall_timeout, has stopped reading or vanished: it is reset. Looked at every stall_check
    // while output waits. This is TCP_USER_TIMEOUT's 20 s, counted by the service, not the
    // kernel: Linux ends a reader that frees its window a little at a time as if it had stopped
    // (net::clear_user_timeout), while here any read that lets output through counts.
    core::Millis stall_timeout{20'000};
    core::Millis stall_check{1'000};
    // Clients get a Close 1001 and this long to answer it before a drain cuts them off.
    core::Millis drain_deadline{5'000};
    // Per client, as the gateway's (ADR-0052, ADR-0076), so that one address or one account
    // cannot take the node's max_connections from everyone else. A peer's connections, from
    // accept to close, while it connects directly; behind a trusted proxy, the upgrades from one
    // forwarded address that have not been answered yet. 20: a household behind one NAT, each
    // person with a phone and a few tabs. A socket counts against its address for its life only
    // when it comes directly: through the proxy a carrier-grade NAT puts hundreds of users on one
    // address, and once a token is verified the user's own cap governs.
    std::size_t max_connections_per_ip = 20;
    // New connections a second per direct peer, 10 saved: each is a handshake, a token check
    // and a parser; a client that reconnects in a loop is slowed, not the node.
    std::uint32_t new_connections_per_ip_per_second = 10;
    // Open sockets per user on this node. A person has a phone, a laptop and a few tabs, each
    // with a socket; 16 covers that twice over, so it takes 80 users to fill the node's 1280,
    // not one token.
    std::size_t max_sessions_per_user = 16;
    // Peers whose X-Forwarded-For names the client (ULW_TRUSTED_PROXIES), and how many proxies
    // stand in front: the client is that many entries from the right (ADR-0052).
    // Initialised so that a designated initializer may leave it out; GCC's
    // -Wmissing-field-initializers flags it otherwise.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    std::vector<net::IpNetwork> trusted_proxies = {};
    std::size_t trusted_proxy_hops = 1;
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
    core::ports::IMessageStore& messages;
    core::ports::IJwtVerifier& verifier;
    const core::ports::IClock& clock;
    // Seeds the per-client tables' hashes, which clients choose the keys of.
    core::ports::IRandom& random;
};

struct Counters {
    std::uint64_t connections_accepted = 0;
    std::uint64_t connections_rejected = 0;
    // Gone before it was served, or not an IP socket: its peer address could not be read.
    std::uint64_t connections_unaddressed = 0;
    std::uint64_t upgrades = 0;
    std::uint64_t auth_failures = 0;
    std::uint64_t origin_rejections = 0;
    std::uint64_t messages_received = 0;
    std::uint64_t protocol_errors = 0;
    std::uint64_t control_floods = 0;
    std::uint64_t slow_consumers = 0;
    std::uint64_t stalled_readers = 0;
    // Sockets closed with kTokenExpired because the token they were opened with ran out.
    std::uint64_t token_expiries = 0;
    std::uint64_t allocation_failures = 0;
    // Reset at accept: the peer's address had max_connections_per_ip open, or opened
    // new_connections_per_ip_per_second too many.
    std::uint64_t rejected_ip_connections = 0;
    std::uint64_t rejected_ip_rate = 0;
    // Upgrades answered 429: a forwarded address with max_connections_per_ip unanswered, or a
    // user with max_sessions_per_user open.
    std::uint64_t limited_ip_upgrades = 0;
    std::uint64_t limited_user_sessions = 0;
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
    // Sessions still holding an HTTP parser: those whose request has not been answered yet.
    [[nodiscard]] std::size_t http_parsers() const noexcept;
    // Every session not yet retired, for a test that acts on one from the reactor thread.
    template <class Fn> void for_each_session(Fn fn) { sessions_.for_each_live(fn); }
    [[nodiscard]] std::string render_metrics() const;

    [[nodiscard]] const Deps& deps() const noexcept { return deps_; }
    [[nodiscard]] const Access& access() const noexcept { return access_; }
    [[nodiscard]] const Limits& limits() const noexcept { return limits_; }
    [[nodiscard]] Counters& counters() noexcept { return counters_; }
    [[nodiscard]] ChatService& chat() noexcept { return chat_; }
    [[nodiscard]] Presence& presence() noexcept { return presence_; }
    [[nodiscard]] Session* session(net::Slab<Session>::Handle handle) noexcept;
    void retire(net::Slab<Session>::Handle handle) noexcept;

    // A slot counted against an address or a user, held until given back.
    using Hold = std::uint32_t;
    [[nodiscard]] bool trusted_proxy(const net::IpAddress& peer) const noexcept;
    // Counts one more connection against a forwarded client's address; nullopt when it has
    // max_connections_per_ip already.
    [[nodiscard]] std::optional<Hold> hold_client(const net::IpAddress& client) noexcept;
    void release_client(Hold hold) noexcept;
    // Counts one more open socket against the user; nullopt at max_sessions_per_user.
    [[nodiscard]] std::optional<Hold> hold_user(const core::UserId& user) noexcept;
    void release_user(Hold hold) noexcept;

private:
    struct ClientEntry {
        TokenBucket new_connections;
    };
    struct UserEntry {};

    // A direct peer's connection: its address's count and its rate, at accept.
    [[nodiscard]] std::optional<Hold> admit_peer(const net::IpAddress& peer) noexcept;

    Deps deps_;
    Access access_;
    Limits limits_;
    Counters counters_;
    RouterRooms rooms_;
    // Sessions detach from both as they close, so they outlive them.
    ChatService chat_;
    Presence presence_;
    http::BoundedTable<net::IpAddress, ClientEntry, http::AddressHash> clients_;
    http::BoundedTable<core::UserId, UserEntry, http::ViewHash> users_;
    net::Slab<Session> sessions_;
    bool draining_ = false;
    bool released_ = false;
    net::TimerId drain_timer_;
};

} // namespace chat
