#pragma once

#include "core/models/ids.hpp"
#include "core/util/time.hpp"
#include "rt/message_key.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace rt {

// An owner renews its rooms this often. Seconds, not minutes: a room is unavailable from its
// owner's death until the stale bound below has passed, and a beat costs one statement.
inline constexpr core::Millis kOwnerHeartbeat{1'000};
// A store call that has not answered by then is abandoned. Every statement touches a handful
// of rows by primary key and answers in about a millisecond; two seconds is a server that is
// stuck, and waiting longer would only hold a beat past the next one.
inline constexpr core::Millis kStoreTimeout{2'000};
// A room whose heartbeat is older than this may be taken from its owner. A healthy owner whose
// beat is stuck and times out (2 s) beats again at the next tick, so its heartbeat is never
// older than 1 + 2 + 1 = 4 s; the fifth second is margin for scheduling. heartbeat_at and the
// comparison both use the database's clock, so skew between nodes does not enter.
inline constexpr core::Millis kOwnerStaleAfter{5'000};

// Rooms whose writes are sequenced and fanned out like any other's but never kept: RFC 9562
// version 8 ids, which chat_server derives for presence (ADR-0053) and no client can name. A
// store takes their seqs, fenced as ever, and stores no message for them.
[[nodiscard]] inline bool is_ephemeral_room(const core::RoomId& room) noexcept {
    return (room.uuid().bytes()[6] & std::byte{0xF0}) == std::byte{0x80};
}

struct Ownership {
    core::NodeId node;
    std::uint64_t generation = 0;
    // The room's last_seq as the answer that made or gave the room to the asking node found it;
    // 0 in any other answer. A new owner starts counting its head from it, not from 0.
    std::uint64_t last_seq = 0;

    // Who holds the room, under which generation: last_seq describes an answer, not the owner.
    friend bool operator==(const Ownership& a, const Ownership& b) noexcept {
        return a.node == b.node && a.generation == b.generation;
    }
};

struct OwnedRoom {
    core::RoomId room;
    std::uint64_t generation = 0;
    // As Ownership::last_seq, in the answer to claim_stale.
    std::uint64_t last_seq = 0;

    friend bool operator==(const OwnedRoom&, const OwnedRoom&) = default;
};

enum class StoreError : std::uint8_t {
    // Unreachable, timed out, or lost a race it cannot settle; a later call may succeed.
    Unavailable,
    // A stored value that no writer here produces.
    Corrupt,
    // Another run of the same node, still alive, holds the node's name.
    NodeTaken,
};

// A message on its way to its sequence number. The body is a view, valid only during the call
// that carries it.
struct Outgoing {
    core::UserId sender;
    MessageKey key;
    std::span<const std::byte> body;
};

template <class T> using StoreResult = std::expected<T, StoreError>;
template <class T> using StoreCallback = std::move_only_function<void(StoreResult<T>) noexcept>;

// Called on the reactor thread.
class IOwnershipListener {
public:
    virtual ~IOwnershipListener() = default;
    // A room was created or changed hands. Only a hint: it may arrive late, or not at all.
    virtual void on_owner_changed(const core::RoomId& room, const Ownership& owner) noexcept = 0;
    // Changes may have gone unreported since the last call; nothing learnt from them holds.
    virtual void on_resync() noexcept = 0;
};

// Where rooms' owners, generations and sequence numbers are kept (ADR-0015), and where nodes
// publish the address of their node channel. Every call answers on the reactor thread, never
// from inside the call itself. Calls outstanding when the store is destroyed are dropped
// unanswered.
class IRoomStore {
public:
    virtual ~IRoomStore() = default;

    // At most one listener, for as long as the store lives.
    virtual void watch(IOwnershipListener& listener) noexcept = 0;

    // The room's owner. `node` takes the room, under a new generation, when the room does not
    // exist yet, when its owner's heartbeat is older than kOwnerStaleAfter, or when it is
    // recorded as `node`'s from before `node`'s current run advertised: that earlier run may
    // have written under the recorded generation, and must be fenced out.
    virtual void resolve(const core::RoomId& room, const core::NodeId& node,
                         StoreCallback<Ownership> done) = 0;
    // Takes each of `rooms` whose owner's heartbeat is stale; answers with those it took.
    virtual void claim_stale(std::vector<core::RoomId> rooms, const core::NodeId& node,
                             StoreCallback<std::vector<OwnedRoom>> done) = 0;
    // An owner write: renews each room `node` still holds at the given generation, and answers
    // with those. A room missing from the answer was fenced out. Also keeps `incarnation`'s
    // hold on the node's name.
    virtual void heartbeat(const core::NodeId& node, const core::Uuid& incarnation,
                           std::vector<OwnedRoom> rooms,
                           StoreCallback<std::vector<core::RoomId>> done) = 0;
    // An owner write: the room's next sequence number for `message`, or nullopt when
    // `generation` is no longer the room's. Then nothing was written. A store that keeps
    // messages keeps this one in the same fenced write, so that no seq is ever taken without
    // its message (ADR-0043).
    virtual void append(const core::RoomId& room, std::uint64_t generation, const Outgoing& message,
                        StoreCallback<std::optional<std::uint64_t>> done) = 0;
    // An owner write, for a node about to stop: the rooms it still holds at these generations
    // become claimable at once instead of after kOwnerStaleAfter.
    virtual void release(const core::NodeId& node, std::vector<OwnedRoom> rooms,
                         StoreCallback<void> done) = 0;

    // Records the numeric host:port where `node` takes node-channel connections, and starts
    // `incarnation`, one run of the node. NodeTaken while another incarnation of the same name
    // has heartbeated within kOwnerStaleAfter: two live processes under one name would each
    // take the other's rooms as their own, unfenced. Advertising again under the same
    // incarnation changes only the address.
    virtual void advertise(const core::NodeId& node, std::string address,
                           const core::Uuid& incarnation, StoreCallback<void> done) = 0;
    // The recorded owner of each of `rooms` that exists, read only: nothing is created, claimed
    // or announced.
    virtual void
    read_owners(std::vector<core::RoomId> rooms,
                StoreCallback<std::vector<std::pair<core::RoomId, Ownership>>> done) = 0;
    // nullopt for a node that never advertised.
    virtual void find_address(const core::NodeId& node,
                              StoreCallback<std::optional<std::string>> done) = 0;
};

} // namespace rt
