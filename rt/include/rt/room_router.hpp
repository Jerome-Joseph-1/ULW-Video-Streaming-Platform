#pragma once

#include "core/models/ids.hpp"
#include "core/ports/clock.hpp"
#include "core/ports/random.hpp"
#include "net/reactor.hpp"
#include "os/unique_fd.hpp"
#include "rt/message_key.hpp"
#include "rt/registry.hpp"
#include "rt/room_store.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rt {

// A message as its room's owner sequenced it. The body is opaque and valid only during the
// call that carries it.
struct Message {
    core::RoomId room;
    std::uint64_t seq = 0;
    core::UserId sender;
    MessageKey key;
    std::span<const std::byte> body;
};

// A client connection on this node. Called on the reactor thread; it must not call back into
// the router from inside deliver().
class IMember {
public:
    virtual ~IMember() = default;
    virtual void deliver(const Message& message) noexcept = 0;
};

enum class RouteError : std::uint8_t {
    // The member has not joined the room.
    NotJoined,
    // The owner's write was fenced: the room changed hands under it, and the message was
    // neither sequenced nor delivered. Sending again reaches the new owner.
    Fenced,
    // No owner could be reached, or it did not answer in time. A message may or may not have
    // been sequenced; the owner never retries on its own (ADR-0035).
    Unavailable,
    // The room's owner has more writes queued than it takes.
    Busy,
};

// A join's answer: the room's latest seq known here, the head a member that missed messages
// compares its last seq against.
using JoinCallback =
    std::move_only_function<void(std::expected<std::uint64_t, RouteError>) noexcept>;
using SendCallback =
    std::move_only_function<void(std::expected<std::uint64_t, RouteError>) noexcept>;

// What the router reports for the log and the metrics. Called on the reactor thread.
class IRouterEvents {
public:
    virtual ~IRouterEvents() = default;
    // An owner write updated no rows (ADR-0015); nothing was applied or delivered.
    virtual void on_fenced_out(const core::RoomId& room, std::uint64_t generation,
                               OwnerWrite write) noexcept = 0;
    // This node became the room's owner: created it (generation 1) or took it over.
    virtual void on_took_room(const core::RoomId& room, std::uint64_t generation) noexcept = 0;
    // The connection to another node failed; the rooms routed through it are looked up again.
    virtual void on_peer_lost(const core::NodeId& peer) noexcept = 0;
    // A node-channel peer failed the handshake: it does not hold the secret, or is not the
    // node it was dialled as. Nothing it sent was acted on.
    virtual void on_peer_refused(std::string_view why) noexcept = 0;
    // Another live process runs under this node's name. This one takes no room and stays
    // unready, trying again each beat, until the other stops.
    virtual void on_node_taken() noexcept = 0;
};

// A node secret shorter than this is refused at startup: HMAC-SHA256 is as strong as its key,
// up to the hash's own 32 bytes.
inline constexpr std::size_t kMinNodeSecretBytes = 32;

struct RouterConfig {
    core::NodeId self;
    // The numeric host:port other nodes dial, published through the store.
    std::string advertise;
    // Shared by every node of the deployment; each end of a node-channel connection proves it
    // holds it before anything else is exchanged (ADR-0035). kMinNodeSecretBytes at least.
    std::string secret;
    // A room this node owns that has had no members here, no subscribed node and no write for
    // this long is given up, so that rooms nobody uses do not pile up on their owner. A minute
    // outlasts a reconnect; a room used again is simply taken again.
    core::Millis idle_release{60'000};
    // Rooms with members on this node at once. The owner heartbeat names every owned room once
    // a second, and each costs a few hundred bytes here: 16384 rooms keep the beat near 1 MB and
    // the bookkeeping to a few MB. A join past it is answered Busy.
    std::size_t max_rooms = 16'384;
    // How long a room can stay routed to an owner that let it go, if the notification saying so
    // was lost: all such rooms' owners are read again this often, in one statement.
    core::Millis revalidate_every{10'000};
};

struct RouterCounters {
    std::uint64_t forwarded = 0;
    std::uint64_t forward_timeouts = 0;
    std::uint64_t peers_lost = 0;
    std::uint64_t delivered = 0;
    // Node-channel connections closed because an allocation failed while reading from them.
    std::uint64_t allocation_failures = 0;
    // Node-channel handshakes that failed.
    std::uint64_t peers_refused = 0;
    // Unfinished node-channel handshakes dropped to make room for a newer one.
    std::uint64_t handshakes_evicted = 0;
    // Node-channel connections closed because the other end stopped reading.
    std::uint64_t slow_peers = 0;
    // Sends answered with the seq their key already had, instead of being sequenced again.
    std::uint64_t duplicates = 0;
};

// One node's share of the room plane (ADR-0015, ADR-0035). Members join rooms here, wherever
// the room's owner is; every mutation goes to the owner, which sequences it with a fenced
// append and fans it out: to its own members directly, and to every node with members, over
// the node channel, for them to deliver to theirs. Single-threaded, like the reactor.
//
// Callbacks may run inside the call that takes them when the answer is known at once.
class RoomRouter {
public:
    // `random` makes the handshake nonces, and must be unpredictable to other nodes.
    RoomRouter(net::IReactor& reactor, IRoomStore& store, const core::ports::IClock& clock,
               core::ports::IRandom& random, RouterConfig config, IRouterEvents& events);
    // The store must be destroyed before the router, the reactor after it.
    ~RoomRouter();
    RoomRouter(const RoomRouter&) = delete;
    RoomRouter& operator=(const RoomRouter&) = delete;
    RoomRouter(RoomRouter&&) = delete;
    RoomRouter& operator=(RoomRouter&&) = delete;

    // Takes node-channel connections on `listener` and publishes this node's address.
    [[nodiscard]] std::expected<void, int> start(os::UniqueFd listener);

    // Answers once the room's owner is known and, if it is another node, has taken this
    // node's subscription: from then on the member receives every message the owner
    // sequences. The answer is the latest seq the owner took or this node delivered. After a
    // takeover it can lag a seq the old owner took and never delivered; the next delivery
    // shows that gap.
    void join(const core::RoomId& room, IMember& member, JoinCallback done);
    // Also drops the member's join still in progress and its sends not answered yet, whose
    // callbacks are then never called; a send already on its way is still sequenced.
    void leave(const core::RoomId& room, IMember& member) noexcept;
    // Answers with the message's sequence number once its owner has sequenced it. A message
    // whose sender and key were sequenced lately, as seen here or by the owner, is not
    // sequenced again: the answer is the seq it got the first time.
    void send(const core::RoomId& room, IMember& from, const core::UserId& sender,
              const MessageKey& key, std::vector<std::byte> body, SendCallback done);

    // For a drain: stops owning rooms and makes them claimable at once.
    void release_rooms(StoreCallback<void> done);
    // Destroys closed node-channel connections the kernel has let go of. Call after each
    // run_once.
    void reap() noexcept;

    // The address is published and the owner heartbeat reaches the store.
    [[nodiscard]] bool healthy() const noexcept;
    [[nodiscard]] std::size_t rooms_owned() const noexcept;
    [[nodiscard]] std::size_t rooms_joined() const noexcept;
    [[nodiscard]] const RegistryCounters& registry_counters() const noexcept;
    [[nodiscard]] const RouterCounters& counters() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rt
