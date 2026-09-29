#include "chat.hpp"

#include "net/socket.hpp"

#include "log.hpp"
#include "session.hpp"

#include <algorithm>
#include <chrono>
#include <format>

namespace chat {

namespace {

std::string_view write_name(rt::OwnerWrite write) noexcept {
    switch (write) {
    case rt::OwnerWrite::Append:
        return "append";
    case rt::OwnerWrite::Heartbeat:
        return "heartbeat";
    }
    return "append";
}

} // namespace

// Which owner write was refused (ADR-0015), under which generation. Refused means it updated
// no row; how many rows a write touched is the database's to show, not this line's to claim.
void RoomLog::on_fenced_out(const core::RoomId& room, std::uint64_t generation,
                            rt::OwnerWrite write) noexcept {
    log_event(R"("level":"warn","msg":"fenced out","node":"{}","room":"{}","generation":{},)"
              R"("write":"{}")",
              self_.view(), room.to_string(), generation, write_name(write));
}

void RoomLog::on_took_room(const core::RoomId& room, std::uint64_t generation) noexcept {
    log_event(R"("level":"info","msg":"owning room","node":"{}","room":"{}","generation":{})",
              self_.view(), room.to_string(), generation);
}

void RoomLog::on_peer_lost(const core::NodeId& peer) noexcept {
    log_event(R"("level":"warn","msg":"peer lost","node":"{}","peer":"{}")", self_.view(),
              peer.view());
}

// `why` is one of the router's fixed reasons; nothing a peer sent is echoed.
void RoomLog::on_peer_refused(std::string_view why) noexcept {
    log_event(R"("level":"warn","msg":"node refused","node":"{}","why":"{}")", self_.view(), why);
}

void RoomLog::on_node_taken() noexcept {
    log_event(R"("level":"error","msg":"node name in use by another live process","node":"{}")",
              self_.view());
}

ChatServer::ChatServer(Deps deps, Access access, Limits limits)
    : deps_(deps), access_(std::move(access)), limits_(limits), sessions_(limits_.max_connections) {
}

ChatServer::~ChatServer() {
    deps_.reactor.cancel_timer(drain_timer_);
    sessions_.for_each_live([](Session& s) { s.close(); });
    reap();
}

void ChatServer::on_accept(os::UniqueFd conn) noexcept {
    if (draining_) {
        return;
    }
    if (!net::tune_connection(conn.get())) {
        ++counters_.connections_rejected;
        return;
    }
    const auto handle = sessions_.emplace(*this);
    if (!handle) {
        ++counters_.connections_rejected;
        return;
    }
    Session* s = sessions_.get(*handle);
    auto id = deps_.reactor.attach(std::move(conn), *s);
    if (!id) {
        ++counters_.connections_rejected;
        sessions_.retire(*handle);
        return;
    }
    ++counters_.connections_accepted;
    s->start(*id);
}

void ChatServer::on_signal(net::Signal signal) noexcept {
    switch (signal) {
    case net::Signal::Terminate:
        begin_drain();
        return;
    case net::Signal::Reload:
        return;
    }
}

void ChatServer::begin_drain() noexcept {
    if (draining_) {
        return;
    }
    draining_ = true;
    deps_.reactor.stop_listening();
    sessions_.for_each_live([](Session& s) { s.drain(); });
    // A release that fails leaves the rooms to go stale, which costs the other nodes
    // rt::kOwnerStaleAfter; the drain goes on either way.
    deps_.router.release_rooms([this](rt::StoreResult<void>) noexcept { released_ = true; });
    drain_timer_ = deps_.reactor.arm_timer(limits_.drain_deadline, *this);
}

void ChatServer::on_timeout() noexcept {
    drain_timer_ = {};
    released_ = true;
    sessions_.for_each_live([](Session& s) { s.close(); });
}

void ChatServer::reap() noexcept {
    sessions_.reap([this](Session& s) { return deps_.reactor.is_quiescent(s.conn()); });
    deps_.router.reap();
}

Session* ChatServer::session(net::Slab<Session>::Handle handle) noexcept {
    return sessions_.get(handle);
}

void ChatServer::retire(net::Slab<Session>::Handle handle) noexcept {
    sessions_.retire(handle);
}

bool ChatServer::admit_join(const core::UserId& user) {
    const core::MonoTime now = deps_.reactor.now();
    const auto refill = [&](JoinBucket& b) {
        const auto elapsed = std::chrono::duration_cast<core::Seconds>(now - b.refilled).count();
        if (elapsed <= 0) {
            return;
        }
        b.tokens = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            limits_.join_burst,
            b.tokens + (static_cast<std::uint64_t>(elapsed) * limits_.joins_per_second)));
        b.refilled += core::Seconds{elapsed};
    };
    // A full bucket is the same as none, so users idle long enough to refill are forgotten
    // whenever the table outgrows the connections that could be using it.
    if (joins_.size() > limits_.max_connections) {
        std::erase_if(joins_, [&](auto& entry) {
            refill(entry.second);
            return entry.second.tokens >= limits_.join_burst;
        });
    }
    auto [it, fresh] =
        joins_.try_emplace(user, JoinBucket{.tokens = limits_.join_burst, .refilled = now});
    JoinBucket& bucket = it->second;
    refill(bucket);
    if (bucket.tokens == 0) {
        return false;
    }
    --bucket.tokens;
    return true;
}

std::string ChatServer::render_metrics() const {
    const Counters& c = counters_;
    const rt::RegistryCounters& registry = deps_.router.registry_counters();
    const rt::RouterCounters& router = deps_.router.counters();
    return std::format("connections_accepted_total {}\n"
                       "connections_rejected_total{{reason=\"capacity\"}} {}\n"
                       "connections_current {}\n"
                       "websocket_upgrades_total {}\n"
                       "auth_failures_total {}\n"
                       "origin_rejections_total {}\n"
                       "messages_received_total {}\n"
                       "messages_delivered_total {}\n"
                       "protocol_errors_total {}\n"
                       "control_floods_total {}\n"
                       "slow_consumers_total {}\n"
                       "allocation_failures_total {}\n"
                       "rooms_active {}\n"
                       "rooms_joined {}\n"
                       "room_reassignments_total {}\n"
                       "fenced_writes_total {}\n"
                       "forwards_total {}\n"
                       "forward_timeouts_total {}\n"
                       "peers_lost_total {}\n"
                       "peers_refused_total {}\n"
                       "slow_peers_total {}\n",
                       c.connections_accepted, c.connections_rejected, sessions_.size(), c.upgrades,
                       c.auth_failures, c.origin_rejections, c.messages_received,
                       c.messages_delivered, c.protocol_errors, c.control_floods, c.slow_consumers,
                       c.allocation_failures + router.allocation_failures,
                       deps_.router.rooms_owned(), deps_.router.rooms_joined(),
                       registry.reassignments, registry.fenced_writes, router.forwarded,
                       router.forward_timeouts, router.peers_lost, router.peers_refused,
                       router.slow_peers);
}

} // namespace chat
