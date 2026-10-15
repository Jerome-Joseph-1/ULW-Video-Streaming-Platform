#pragma once

#include "core/ports/clock.hpp"
#include "core/ports/message_store.hpp"
#include "core/ports/notify.hpp"
#include "net/reactor.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

namespace infra::notify {

// The connectivity registry without a database, for tests (ADR-0111). Same contract as the
// durable one: results arrive on a later loop iteration; time is the injected clock's. The rooms
// message_targets() reads are the ones set_room() lists, standing in for the message store's.
class MemoryConnectivity final : public core::ports::IConnectivityStore, public net::ITimerHandler {
public:
    MemoryConnectivity(net::IReactor& reactor, const core::ports::IClock& clock,
                       core::ports::NotifyTiming timing);
    ~MemoryConnectivity() override;
    MemoryConnectivity(const MemoryConnectivity&) = delete;
    MemoryConnectivity& operator=(const MemoryConnectivity&) = delete;
    MemoryConnectivity(MemoryConnectivity&&) = delete;
    MemoryConnectivity& operator=(MemoryConnectivity&&) = delete;

    // The room's kind and member list, as the message store has them.
    void set_room(const core::RoomId& room, core::ports::RoomKind kind,
                  std::vector<core::UserId> members);
    // Every call answers Unavailable while set.
    void fail(bool failing) noexcept { failing_ = failing; }

    void renew(std::uint64_t node, core::ports::NotifyCallback<bool> done) override;
    void sync(std::uint64_t node, std::vector<core::ports::SocketDevice> added,
              std::vector<core::ports::SocketDevice> removed,
              core::ports::NotifyCallback<void> done) override;
    void forget(std::uint64_t node, core::ports::NotifyCallback<void> done) override;
    void
    connected(std::vector<core::UserId> users,
              core::ports::NotifyCallback<std::vector<core::ports::SocketDevice>> done) override;
    void message_targets(const core::RoomId& room, const core::UserId& sender,
                         core::ports::NotifyCallback<std::vector<core::UserId>> done) override;

    void on_timeout() noexcept override;

private:
    struct Node {
        core::MonoTime renewed_at;
        std::vector<core::ports::SocketDevice> sockets;
    };
    struct Room {
        core::ports::RoomKind kind = core::ports::RoomKind::GroupChat;
        std::vector<core::UserId> members;
    };

    [[nodiscard]] bool live(const Node& node, core::MonoTime now) const noexcept;
    [[nodiscard]] bool connected_now(const core::UserId& user, core::MonoTime now) const;
    template <class T>
    void reply(core::ports::NotifyCallback<T> done, core::ports::NotifyResult<T> r);
    void defer(std::move_only_function<void() noexcept> fn);

    net::IReactor& reactor_;
    const core::ports::IClock& clock_;
    core::ports::NotifyTiming timing_;
    bool failing_ = false;
    std::unordered_map<std::uint64_t, Node> nodes_;
    std::unordered_map<core::RoomId, Room> rooms_;
    // When each member was last told of each room.
    std::unordered_map<core::RoomId, std::unordered_map<core::UserId, core::MonoTime>> marks_;
    std::vector<std::move_only_function<void() noexcept>> pending_;
    net::TimerId timer_;
};

} // namespace infra::notify
