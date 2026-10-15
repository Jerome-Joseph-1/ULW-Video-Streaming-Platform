#include "infra/notify/memory_connectivity.hpp"

#include <algorithm>
#include <utility>

namespace infra::notify {

using core::ports::NotifyCallback;
using core::ports::NotifyError;
using core::ports::NotifyResult;
using core::ports::SocketDevice;

MemoryConnectivity::MemoryConnectivity(net::IReactor& reactor, const core::ports::IClock& clock,
                                       core::ports::NotifyTiming timing)
    : reactor_(reactor), clock_(clock), timing_(timing) {}

MemoryConnectivity::~MemoryConnectivity() {
    reactor_.cancel_timer(timer_);
}

void MemoryConnectivity::set_room(const core::RoomId& room, core::ports::RoomKind kind,
                                  std::vector<core::UserId> members) {
    rooms_[room] = Room{.kind = kind, .members = std::move(members)};
}

void MemoryConnectivity::defer(std::move_only_function<void() noexcept> fn) {
    pending_.push_back(std::move(fn));
    if (timer_ == net::TimerId{}) {
        timer_ = reactor_.arm_timer(core::Millis{0}, *this);
    }
}

void MemoryConnectivity::on_timeout() noexcept {
    timer_ = {};
    // Callbacks may issue further calls; those land in a fresh batch for the next iteration.
    std::vector<std::move_only_function<void() noexcept>> batch;
    batch.swap(pending_);
    for (auto& fn : batch) {
        fn();
    }
}

template <class T> void MemoryConnectivity::reply(NotifyCallback<T> done, NotifyResult<T> r) {
    defer([done = std::move(done), r = std::move(r)]() mutable noexcept { done(std::move(r)); });
}

bool MemoryConnectivity::live(const Node& node, core::MonoTime now) const noexcept {
    return now - node.renewed_at < timing_.lease;
}

bool MemoryConnectivity::connected_now(const core::UserId& user, core::MonoTime now) const {
    return std::ranges::any_of(nodes_, [&](const auto& n) {
        return live(n.second, now) &&
               std::ranges::any_of(n.second.sockets,
                                   [&](const SocketDevice& s) { return s.user == user; });
    });
}

void MemoryConnectivity::renew(std::uint64_t node, NotifyCallback<bool> done) {
    if (failing_) {
        reply<bool>(std::move(done), std::unexpected(NotifyError::Unavailable));
        return;
    }
    const core::MonoTime now = clock_.now();
    const bool made = !nodes_.contains(node);
    nodes_[node].renewed_at = now;
    std::erase_if(nodes_, [&](const auto& n) {
        return n.first != node && now - n.second.renewed_at >= timing_.forget_after;
    });
    for (auto& [room, marks] : marks_) {
        std::erase_if(marks,
                      [&](const auto& m) { return now - m.second >= timing_.message_interval; });
    }
    std::erase_if(marks_, [](const auto& r) { return r.second.empty(); });
    reply<bool>(std::move(done), made);
}

void MemoryConnectivity::sync(std::uint64_t node, std::vector<SocketDevice> added,
                              std::vector<SocketDevice> removed, NotifyCallback<void> done) {
    const auto found = nodes_.find(node);
    const bool overlap = std::ranges::any_of(added, [&](const SocketDevice& a) {
        return std::ranges::find(removed, a) != removed.end();
    });
    if (failing_ || found == nodes_.end() || overlap ||
        added.size() + removed.size() > core::ports::kMaxSyncRows) {
        reply<void>(std::move(done), std::unexpected(NotifyError::Unavailable));
        return;
    }
    auto& sockets = found->second.sockets;
    for (const SocketDevice& r : removed) {
        std::erase(sockets, r);
    }
    for (SocketDevice& a : added) {
        for (auto& [room, marks] : marks_) {
            marks.erase(a.user);
        }
        if (std::ranges::find(sockets, a) == sockets.end()) {
            sockets.push_back(std::move(a));
        }
    }
    reply<void>(std::move(done), {});
}

void MemoryConnectivity::forget(std::uint64_t node, NotifyCallback<void> done) {
    if (failing_) {
        reply<void>(std::move(done), std::unexpected(NotifyError::Unavailable));
        return;
    }
    nodes_.erase(node);
    reply<void>(std::move(done), {});
}

void MemoryConnectivity::connected(std::vector<core::UserId> users,
                                   NotifyCallback<std::vector<SocketDevice>> done) {
    if (failing_ || users.size() > core::ports::kMaxConnectedQuery) {
        reply<std::vector<SocketDevice>>(std::move(done),
                                         std::unexpected(NotifyError::Unavailable));
        return;
    }
    const core::MonoTime now = clock_.now();
    std::vector<SocketDevice> out;
    for (const auto& [tag, node] : nodes_) {
        if (!live(node, now)) {
            continue;
        }
        for (const SocketDevice& s : node.sockets) {
            if (std::ranges::find(users, s.user) != users.end() &&
                std::ranges::find(out, s) == out.end()) {
                out.push_back(s);
            }
        }
    }
    reply<std::vector<SocketDevice>>(std::move(done), std::move(out));
}

void MemoryConnectivity::message_targets(const core::RoomId& room, const core::UserId& sender,
                                         NotifyCallback<std::vector<core::UserId>> done) {
    if (failing_) {
        reply<std::vector<core::UserId>>(std::move(done),
                                         std::unexpected(NotifyError::Unavailable));
        return;
    }
    std::vector<core::UserId> due;
    const auto found = rooms_.find(room);
    if (found != rooms_.end() && found->second.kind != core::ports::RoomKind::StreamLiveChat) {
        const core::MonoTime now = clock_.now();
        auto& marks = marks_[room];
        for (const core::UserId& member : found->second.members) {
            if (member == sender || connected_now(member, now)) {
                continue;
            }
            const auto mark = marks.find(member);
            if (mark != marks.end() && now - mark->second < timing_.message_interval) {
                continue;
            }
            marks[member] = now;
            due.push_back(member);
        }
        if (marks.empty()) {
            marks_.erase(room);
        }
        std::ranges::sort(due, {}, &core::UserId::view);
    }
    reply<std::vector<core::UserId>>(std::move(done), std::move(due));
}

} // namespace infra::notify
