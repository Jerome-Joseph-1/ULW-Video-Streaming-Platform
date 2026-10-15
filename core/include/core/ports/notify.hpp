#pragma once

#include "core/models/ids.hpp"
#include "core/util/time.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

// Who has a socket open where, for the notification hook (ADR-0111): a registry each chat node
// writes its sockets' devices to, under a lease it renews, and reads to tell who a call or a
// message should be pushed to.
namespace core::ports {

enum class NotifyError : std::uint8_t {
    // The store is unreachable or timed out; the call may be retried.
    Unavailable,
    // A stored row violates an invariant.
    Corrupt,
};

[[nodiscard]] std::string_view to_string(NotifyError e) noexcept;

template <class T> using NotifyResult = std::expected<T, NotifyError>;

// Called exactly once, on the reactor thread, never from inside the call that was given it.
template <class T> using NotifyCallback = std::move_only_function<void(NotifyResult<T>) noexcept>;

// A socket's user and the device it named when it connected (nullopt: none).
struct SocketDevice {
    UserId user;
    std::optional<DeviceId> device;

    friend bool operator==(const SocketDevice&, const SocketDevice&) = default;
};

// How long a node's rows count after its last renewal, and how often a member may be told of a
// room's messages while they stay disconnected.
struct NotifyTiming {
    Millis lease{60'000};
    Millis message_interval{30'000};
    // A node not renewed for this long is deleted with its rows, by any node's renewal.
    Millis forget_after{600'000};
};

// The most users connected() is asked about at once: a group call's callees (32) with room.
inline constexpr std::size_t kMaxConnectedQuery = 64;
// The most rows one sync() writes: a node's sockets (1280) with room.
inline constexpr std::size_t kMaxSyncRows = 4'096;

class IConnectivityStore {
public:
    virtual ~IConnectivityStore() = default;

    // This run of a node (its tag, unique per run) is alive: its lease starts again. Answers true
    // when the node had no row (its first renewal, or it was forgotten meanwhile): it has no
    // sockets recorded, and should sync all it has. Also deletes the nodes past
    // NotifyTiming::forget_after, with their rows, and the message marks no interval needs any
    // more.
    virtual void renew(std::uint64_t node, NotifyCallback<bool> done) = 0;
    // The node's sockets changed: `added` now have one there, `removed` no longer. A device in
    // both is a caller's mistake (refused, nothing written). Every user in `added` has their
    // message marks cleared: they are back, and the next time they go they start afresh.
    // The node must have renewed first. At most kMaxSyncRows rows in all.
    virtual void sync(std::uint64_t node, std::vector<SocketDevice> added,
                      std::vector<SocketDevice> removed, NotifyCallback<void> done) = 0;
    // The node is going: its rows go with it.
    virtual void forget(std::uint64_t node, NotifyCallback<void> done) = 0;

    // The sockets of `users` at nodes whose lease stands, each (user, device) once, a socket that
    // named no device included. At most kMaxConnectedQuery users.
    virtual void connected(std::vector<UserId> users,
                           NotifyCallback<std::vector<SocketDevice>> done) = 0;

    // The members of the room owed a notification of a message `sender` posted there: the room is
    // a direct or a group chat; they are on its member list, are not `sender`, have no socket at a
    // node whose lease stands, and were not told of this room within the message interval (or
    // have connected since). Those answered are marked told now: a second call within the
    // interval answers none of them, from whichever node it comes. Ordered by user id.
    virtual void message_targets(const RoomId& room, const UserId& sender,
                                 NotifyCallback<std::vector<UserId>> done) = 0;
};

} // namespace core::ports
