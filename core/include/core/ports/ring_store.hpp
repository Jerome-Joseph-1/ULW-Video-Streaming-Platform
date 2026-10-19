#pragma once

#include "core/models/ids.hpp"
#include "core/util/time.hpp"
#include "core/util/uuid.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <vector>

// The call each room rings, kept by the room's owner where the next owner and every node can
// read it (ADR-0129): what lets a change of owner end a ring out loud, and a socket that connects
// hear its user's rings at once.
namespace core::ports {

enum class RingStoreError : std::uint8_t {
    // The store is unreachable or timed out.
    Unavailable,
    // A stored row violates an invariant.
    Corrupt,
};

template <class T> using RingStoreResult = std::expected<T, RingStoreError>;

// Called exactly once, on the reactor thread, never from inside the call that was given it.
template <class T>
using RingStoreCallback = std::move_only_function<void(RingStoreResult<T>) noexcept>;

struct StoredRing {
    RoomId room;
    // The call's id, a UUID.
    Uuid call;
    UserId caller;
    bool group = false;
    // Who it rings.
    std::vector<UserId> callees;
    WallTime started_at;
    WallTime expires_at;

    friend bool operator==(const StoredRing&, const StoredRing&) = default;
};

// At most this many callees are kept per ring: a group call rings at most 32 of its members.
inline constexpr std::size_t kMaxStoredCallees = 64;
// At most this many rings are answered for one user.
inline constexpr std::size_t kMaxRingsForUser = 32;

class IRingStore {
public:
    virtual ~IRingStore() = default;
    // The room rings this call now, in place of any it rang before.
    virtual void put_ring(StoredRing ring, RingStoreCallback<void> done) = 0;
    // The room's ring ended; nothing when the row is another call's (a newer ring) or gone.
    virtual void end_ring(const RoomId& room, const Uuid& call, RingStoreCallback<void> done) = 0;
    // The room's ring, if it has one that has not passed expires_at.
    virtual void ring_of(const RoomId& room, RingStoreCallback<std::optional<StoredRing>> done) = 0;
    // The rings, not past expires_at, that ring `user`: at most kMaxRingsForUser, the newest
    // first.
    virtual void rings_for(const UserId& user, RingStoreCallback<std::vector<StoredRing>> done) = 0;
};

} // namespace core::ports
