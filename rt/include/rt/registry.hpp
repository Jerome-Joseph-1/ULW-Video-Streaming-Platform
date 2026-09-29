#pragma once

#include "core/models/ids.hpp"
#include "core/ports/clock.hpp"
#include "rt/room_store.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rt {

enum class OwnerWrite : std::uint8_t { Append, Heartbeat };

// Called on the reactor thread, from the store's answers and notifications; never from inside
// a call its user makes into the registry.
class IRegistryObserver {
public:
    virtual ~IRegistryObserver() = default;
    // The owner this node knows for `room` changed. Rooms this node owns are reported when it
    // takes them.
    virtual void on_owner_changed(const core::RoomId& room, const Ownership& owner) noexcept = 0;
    // An owner write under `generation` updated nothing: another node holds the room now, and
    // this one no longer owns it at that generation. Reported for every such write.
    virtual void on_fenced_out(const core::RoomId& room, std::uint64_t generation,
                               OwnerWrite write) noexcept = 0;
};

struct RegistryCounters {
    std::uint64_t fenced_writes = 0;
    // Rooms this node took over from an earlier owner, itself included after a restart.
    std::uint64_t reassignments = 0;
};

enum class AppendError : std::uint8_t {
    // The write updated nothing; another node owns the room.
    Fenced,
    // The store did not answer; whether the write happened is unknown.
    Unavailable,
};

using AppendCallback =
    std::move_only_function<void(std::expected<std::uint64_t, AppendError>) noexcept>;

// Which node owns each room this node cares about, and which rooms it owns itself (ADR-0015).
//
// Ownership is only ever lost through fencing: a node keeps acting as the owner of a room until
// an owner write (a message's sequence number, or the heartbeat that runs every
// kOwnerHeartbeat) updates nothing. A notification that another node took the room updates
// the cache that routing reads, but never the ownership itself, so a node that was paused and
// resumes behind a takeover finds out at its next write, and that write is refused.
//
// Owners of other rooms are cached: filled by lookups, refreshed by the store's notifications,
// and dropped when the store says notifications were missed or when the named owner stops
// answering. The store must be destroyed before the registry: its callbacks point here.
class RoomRegistry final : public IOwnershipListener {
public:
    // `incarnation` names this run of the node; its heartbeats keep the name held.
    RoomRegistry(IRoomStore& store, const core::ports::IClock& clock, core::NodeId self,
                 const core::Uuid& incarnation, IRegistryObserver& observer);
    RoomRegistry(const RoomRegistry&) = delete;
    RoomRegistry& operator=(const RoomRegistry&) = delete;
    RoomRegistry(RoomRegistry&&) = delete;
    RoomRegistry& operator=(RoomRegistry&&) = delete;
    ~RoomRegistry() override = default;

    [[nodiscard]] const core::NodeId& self() const noexcept { return self_; }
    // The generation this node owns `room` under, if it does.
    [[nodiscard]] std::optional<std::uint64_t> owned(const core::RoomId& room) const noexcept;
    [[nodiscard]] std::size_t rooms_owned() const noexcept { return owned_.size(); }
    // The owner to route to without asking the store: a cached other node, or this one when it
    // owns the room. A cache entry naming this node without the ownership to match (a claim
    // whose answer was lost) is not an answer.
    [[nodiscard]] std::optional<Ownership> known_owner(const core::RoomId& room) const noexcept;

    // Asks the store, which may make this node the owner. Concurrent calls for one room share
    // one store call.
    void resolve(const core::RoomId& room, StoreCallback<Ownership> done);
    // Drops the cached owner of `room`, after it failed to answer.
    void forget(const core::RoomId& room) noexcept;

    // Takes the room's next sequence number, fenced on the generation this node owns it
    // under. false, and nothing is written, when this node does not own the room.
    [[nodiscard]] bool append(const core::RoomId& room, const Outgoing& message,
                              AppendCallback done);

    // Rooms with members on this node. Their owners are kept cached and watched: when one goes
    // quiet for kOwnerStaleAfter, this node takes the room over.
    void set_interest(const core::RoomId& room, bool interested);
    // Every kOwnerHeartbeat: renews the rooms this node owns and claims the rooms of interest
    // whose owner has gone quiet. A call finding the previous one still running skips it.
    void tick();
    // For a drain: stops owning every room and makes them claimable at once.
    void release_all(StoreCallback<void> done);
    // Stops owning one room nobody here uses, and makes it claimable at once. A no-op for a
    // room this node does not own.
    void release(const core::RoomId& room);
    [[nodiscard]] std::vector<core::RoomId> owned_rooms() const;

    // The last heartbeat reached the store within kOwnerStaleAfter.
    [[nodiscard]] bool healthy() const noexcept;
    [[nodiscard]] const RegistryCounters& counters() const noexcept { return counters_; }

    void on_owner_changed(const core::RoomId& room, const Ownership& owner) noexcept override;
    void on_resync() noexcept override;

private:
    void resolved(const core::RoomId& room, StoreResult<Ownership> result);
    void acquired(const core::RoomId& room, std::uint64_t generation);
    // Caches `owner` unless a newer generation is known; true when the cache changed.
    bool learn(const core::RoomId& room, const Ownership& owner);
    void fenced(const core::RoomId& room, std::uint64_t generation, OwnerWrite write);
    void beat();
    void sweep();

    IRoomStore& store_;
    const core::ports::IClock& clock_;
    core::NodeId self_;
    core::Uuid incarnation_;
    IRegistryObserver& observer_;
    std::unordered_map<core::RoomId, std::uint64_t> owned_;
    std::unordered_map<core::RoomId, Ownership> cache_;
    std::unordered_map<core::RoomId, std::vector<StoreCallback<Ownership>>> resolving_;
    std::unordered_set<core::RoomId> interest_;
    bool beating_ = false;
    bool sweeping_ = false;
    std::optional<core::MonoTime> last_beat_;
    RegistryCounters counters_;
};

} // namespace rt
