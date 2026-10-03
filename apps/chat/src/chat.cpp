#include "chat.hpp"

#include "net/socket.hpp"

#include "log.hpp"
#include "session.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <utility>

namespace chat {

namespace {

// Addresses of the last second or so: a direct peer's bucket is full again that long after its
// last connection, and a full bucket is worth nothing (ADR-0052's arithmetic).
constexpr std::size_t kClientEntries = 16'384;

std::uint64_t seed(core::ports::IRandom& random) {
    std::array<std::byte, sizeof(std::uint64_t)> bytes{};
    random.fill(bytes);
    return std::bit_cast<std::uint64_t>(bytes);
}

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

// `why` is one of the router's fixed reasons, at most with the version number a peer's hello
// named; nothing else a peer sent is echoed.
void RoomLog::on_peer_refused(std::string_view why) noexcept {
    log_event(R"("level":"warn","msg":"node refused","node":"{}","why":"{}")", self_.view(), why);
}

void RoomLog::on_node_taken() noexcept {
    log_event(R"("level":"error","msg":"node name in use by another live process","node":"{}")",
              self_.view());
}

namespace {

// The service's limits, with the clients it may hold: the server's connections.
ServiceLimits service_limits(const Limits& limits) noexcept {
    ServiceLimits service = limits.service;
    service.max_clients = limits.max_connections;
    return service;
}

} // namespace

ChatServer::ChatServer(Deps deps, Access access, Limits limits)
    : deps_(deps), access_(std::move(access)), limits_(std::move(limits)), rooms_(deps.router),
      chat_(rooms_, deps.messages, deps.clock, service_limits(limits_)),
      presence_(rooms_, deps.reactor, deps.clock, deps.node, limits_.presence),
      calls_(deps.messages, deps.sfu, rooms_, deps.clock, deps.random, limits_.calls),
      // One more than the connections that can pin an entry, so a new client always finds one.
      clients_(std::max(kClientEntries, limits_.max_connections + 1),
               http::AddressHash{http::SeededHash(seed(deps_.random))}),
      users_(limits_.max_connections + 1, http::ViewHash{http::SeededHash(seed(deps_.random))}),
      // A block entry holds nothing but its count, so only open connections need one.
      blocks_(limits_.max_connections + 1, http::AddressHash{http::SeededHash(seed(deps_.random))}),
      sessions_(limits_.max_connections) {
    deps_.router.serve(&calls_);
    deps_.router.hear(&bell_);
}

ChatServer::~ChatServer() {
    // The message store is destroyed first (Services in main.cpp), and reap() below sweeps the
    // service, which would otherwise ask it what a resync still owes.
    chat_.stop();
    deps_.router.serve(nullptr);
    deps_.router.hear(nullptr);
    deps_.reactor.cancel_timer(drain_timer_);
    sessions_.for_each_live([](Session& s) { s.close(); });
    reap();
}

void ChatServer::on_accept(os::UniqueFd conn) noexcept {
    if (draining_) {
        return;
    }
    // Gone already, or not an IP socket: nothing to serve either way.
    const auto peer = net::peer_address(conn.get());
    if (!peer) {
        ++counters_.rejected_socket;
        return;
    }
    accept_from(std::move(conn), *peer);
}

void ChatServer::accept_from(os::UniqueFd conn, const net::IpAddress& peer) noexcept {
    // A trusted proxy carries many clients; they are told apart at their upgrade requests.
    std::optional<PeerHold> hold;
    if (!trusted_proxy(peer)) {
        hold = admit_peer(peer);
        if (!hold) {
            // Before a byte is read: no parser, no token check, and no TIME_WAIT (ADR-0052).
            net::reset_connection(std::move(conn));
            return;
        }
    }
    // As the gateway's: a socket that refuses its options, or that the reactor will not take,
    // is the socket's fault; a full slab is the node's.
    const auto refuse = [&](std::uint64_t& counter) {
        ++counter;
        if (hold) {
            release_peer(*hold);
        }
    };
    if (!net::tune_connection(conn.get()) || !net::clear_user_timeout(conn.get()) ||
        !net::cap_send_buffer(conn.get(), limits_.socket_send_buffer)) {
        refuse(counters_.rejected_socket);
        return;
    }
    const auto handle = sessions_.emplace(*this);
    if (!handle) {
        refuse(counters_.rejected_capacity);
        return;
    }
    Session* s = sessions_.get(*handle);
    auto id = deps_.reactor.attach(std::move(conn), *s);
    if (!id) {
        refuse(counters_.rejected_socket);
        sessions_.retire(*handle);
        return;
    }
    ++counters_.connections_accepted;
    s->start(*id, peer, hold);
}

bool ChatServer::trusted_proxy(const net::IpAddress& peer) const noexcept {
    return http::is_trusted_proxy(limits_.trusted_proxies, peer);
}

std::optional<ChatServer::PeerHold> ChatServer::admit_peer(const net::IpAddress& peer) noexcept {
    const core::MonoTime now = deps_.reactor.now();
    const auto slot = clients_.acquire(http::client_key(peer), [&] {
        return ClientEntry{.new_connections =
                               TokenBucket(limits_.new_connections_per_ip_per_second,
                                           limits_.new_connections_per_ip_per_second, now)};
    });
    if (!slot || clients_.pins(*slot) >= limits_.max_connections_per_ip) {
        ++counters_.rejected_ip_connections;
        return std::nullopt;
    }
    // Before the rate is charged: a connection refused for its block costs its /64 nothing.
    std::optional<Hold> block;
    if (const auto key = http::client_block(peer)) {
        block = blocks_.acquire(*key, [] { return BlockEntry{}; });
        if (!block || blocks_.pins(*block) >= limits_.max_connections_per_ip_block) {
            ++counters_.rejected_ip_block;
            return std::nullopt;
        }
    }
    if (!clients_.at(*slot).new_connections.take(now)) {
        ++counters_.rejected_ip_rate;
        return std::nullopt;
    }
    clients_.pin(*slot);
    if (block) {
        blocks_.pin(*block);
    }
    return PeerHold{.address = *slot, .block = block};
}

std::optional<ChatServer::Hold> ChatServer::hold_client(const net::IpAddress& client) noexcept {
    const core::MonoTime now = deps_.reactor.now();
    const auto slot = clients_.acquire(http::client_key(client), [&] {
        return ClientEntry{.new_connections =
                               TokenBucket(limits_.new_connections_per_ip_per_second,
                                           limits_.new_connections_per_ip_per_second, now)};
    });
    if (!slot || clients_.pins(*slot) >= limits_.max_connections_per_ip) {
        ++counters_.limited_ip_upgrades;
        return std::nullopt;
    }
    clients_.pin(*slot);
    return slot;
}

void ChatServer::release_client(Hold hold) noexcept {
    clients_.unpin(hold);
}

void ChatServer::release_peer(PeerHold hold) noexcept {
    clients_.unpin(hold.address);
    if (hold.block) {
        blocks_.unpin(*hold.block);
    }
}

std::optional<ChatServer::Hold> ChatServer::hold_user(const core::UserId& user) noexcept {
    const auto slot = users_.acquire(user, [] { return UserEntry{}; });
    if (!slot || users_.pins(*slot) >= limits_.max_sessions_per_user) {
        ++counters_.limited_user_sessions;
        return std::nullopt;
    }
    users_.pin(*slot);
    return slot;
}

void ChatServer::release_user(Hold hold) noexcept {
    users_.unpin(hold);
}

void ChatServer::on_signal(net::Signal signal) noexcept {
    switch (signal) {
    case net::Signal::Terminate:
        begin_drain();
        return;
    case net::Signal::Reload:
        drop_auth_caches();
        return;
    }
}

// SIGHUP, after the identity provider rotates its signing key (ADR-0082): tokens under the
// withdrawn key stop opening sockets once a key fetch succeeds, not when the cache would next
// refetch; until then the cached keys keep answering. Sockets already open keep running to their
// token's expiry, as they would have anyway (ADR-0073).
void ChatServer::drop_auth_caches() noexcept {
    deps_.verifier.drop_caches();
    ++counters_.auth_cache_drops;
    log_event(R"("level":"info","msg":"auth cache drop requested; completes on the next )"
              R"(successful key fetch","node":"{}")",
              deps_.node.view());
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
    chat_.sweep();
    calls_.sweep();
}

Session* ChatServer::session(net::Slab<Session>::Handle handle) noexcept {
    return sessions_.get(handle);
}

std::size_t ChatServer::http_parsers() const noexcept {
    std::size_t parsing = 0;
    sessions_.for_each_live(
        [&parsing](const Session& s) noexcept { parsing += s.parsing_http() ? 1U : 0U; });
    return parsing;
}

void ChatServer::retire(net::Slab<Session>::Handle handle) noexcept {
    sessions_.retire(handle);
}

std::string ChatServer::render_metrics() const {
    const Counters& c = counters_;
    const rt::RegistryCounters& registry = deps_.router.registry_counters();
    const rt::RouterCounters& router = deps_.router.counters();
    const ServiceCounters& chat = chat_.counters();
    const PresenceCounters& presence = presence_.counters();
    const CallCounters& call = calls_.counters();
    const RingCounters& ring = calls_.ring_counters();
    const BellCounters& bell = bell_.counters();
    return std::format(
        "connections_accepted_total {}\n"
        "connections_rejected_total{{reason=\"capacity\"}} {}\n"
        "connections_rejected_total{{reason=\"socket\"}} {}\n"
        "connections_rejected_total{{reason=\"ip_connections\"}} {}\n"
        "connections_rejected_total{{reason=\"ip_block\"}} {}\n"
        "connections_rejected_total{{reason=\"ip_rate\"}} {}\n"
        "upgrades_limited_total{{limit=\"ip\"}} {}\n"
        "upgrades_limited_total{{limit=\"user_sessions\"}} {}\n"
        "rate_limit_entries{{table=\"client\"}} {}\n"
        "rate_limit_entries{{table=\"user\"}} {}\n"
        "rate_limit_entries{{table=\"ip_block\"}} {}\n"
        "rate_limit_evictions_total{{table=\"client\"}} {}\n"
        "connections_current {}\n"
        "websocket_upgrades_total {}\n"
        "auth_failures_total {}\n"
        "origin_rejections_total {}\n"
        "messages_received_total {}\n"
        "messages_delivered_total {}\n"
        "messages_rate_limited_total {}\n"
        "messages_deduplicated_total {}\n"
        "lossy_drops_total {}\n"
        "messages_replayed_total {}\n"
        "history_messages_total {}\n"
        "messages_kept_bytes {}\n"
        "protocol_errors_total {}\n"
        "control_floods_total {}\n"
        "slow_consumers_total {}\n"
        "stalled_readers_total {}\n"
        "allocation_failures_total {}\n"
        "rooms_active {}\n"
        "rooms_joined {}\n"
        "room_reassignments_total {}\n"
        "fenced_writes_total {}\n"
        "forwards_total {}\n"
        "forward_timeouts_total {}\n"
        "peers_lost_total {}\n"
        "peers_refused_total {}\n"
        "slow_peers_total {}\n"
        "presence_rooms {}\n"
        "presence_events_sent_total {}\n"
        "presence_events_received_total {}\n"
        "presence_notifications_total {}\n"
        "presence_expired_total {}\n"
        "presence_gaps_total {}\n"
        "token_expiries_total {}\n"
        "member_removals_total {}\n"
        "member_check_failures_total {}\n"
        "jwks_keys_expired {}\n"
        "auth_cache_drops_total {}\n"
        "auth_cache_drop_pending {}\n"
        "unrecorded_joins_total {}\n"
        "calls_enabled {}\n"
        "call_tickets_total {}\n"
        "call_refusals_total{{reason=\"not_member\"}} {}\n"
        "call_refusals_total{{reason=\"not_callable\"}} {}\n"
        "call_refusals_total{{reason=\"busy\"}} {}\n"
        "call_rooms_opened_total {}\n"
        "call_rooms {}\n"
        "call_errors_total{{source=\"sfu\",kind=\"unavailable\"}} {}\n"
        "call_errors_total{{source=\"sfu\",kind=\"refused\"}} {}\n"
        "call_errors_total{{source=\"store\",kind=\"unavailable\"}} {}\n"
        "call_refusals_total{{reason=\"no_call\"}} {}\n"
        "call_refusals_total{{reason=\"ring_limited\"}} {}\n"
        "calls_ringing_or_answered {}\n"
        "call_rings_total{{outcome=\"started\"}} {}\n"
        "call_rings_total{{outcome=\"answered\"}} {}\n"
        "call_rings_total{{outcome=\"declined\"}} {}\n"
        "call_rings_total{{outcome=\"cancelled\"}} {}\n"
        "call_rings_total{{outcome=\"missed\"}} {}\n"
        "call_rings_total{{outcome=\"ended\"}} {}\n"
        "call_rings_total{{outcome=\"orphaned\"}} {}\n"
        "call_rings_total{{outcome=\"busy\"}} {}\n"
        "call_rings_total{{outcome=\"graced\"}} {}\n"
        "call_notices_sent_total {}\n"
        "call_events_pushed_total {}\n"
        "call_notices_unheard_total {}\n"
        "call_notices_malformed_total {}\n"
        "notices_total{{stage=\"forwarded\"}} {}\n"
        "notices_total{{stage=\"fanned_out\"}} {}\n"
        "notices_total{{stage=\"heard\"}} {}\n"
        "notices_total{{stage=\"dropped\"}} {}\n",
        c.connections_accepted, c.rejected_capacity, c.rejected_socket, c.rejected_ip_connections,
        c.rejected_ip_block, c.rejected_ip_rate, c.limited_ip_upgrades, c.limited_user_sessions,
        clients_.size(), users_.size(), blocks_.size(), clients_.evictions(), sessions_.size(),
        c.upgrades, c.auth_failures, c.origin_rejections, c.messages_received, chat.delivered,
        chat.rate_limited, router.duplicates, chat.lossy_drops, chat.replayed,
        chat.history_messages, chat_.buffered_bytes(), c.protocol_errors, c.control_floods,
        c.slow_consumers, c.stalled_readers,
        c.allocation_failures + router.allocation_failures + chat.allocation_failures +
            presence.allocation_failures,
        deps_.router.rooms_owned(), deps_.router.rooms_joined(), registry.reassignments,
        registry.fenced_writes, router.forwarded, router.forward_timeouts, router.peers_lost,
        router.peers_refused, router.slow_peers, presence_.rooms(), presence.sent,
        presence.received, presence.notified, presence.expired, presence.gaps, c.token_expiries,
        chat.removals, chat.failed_rechecks, deps_.verifier.keys_expired() ? 1 : 0,
        c.auth_cache_drops, deps_.verifier.drop_pending() ? 1 : 0, chat.unrecorded_joins,
        calls_.enabled() ? 1 : 0, call.tickets, call.not_member, call.not_callable, call.busy,
        call.opens, calls_.rooms(), call.sfu_unavailable, call.sfu_refused, call.store_unavailable,
        call.no_call, ring.limited, calls_.calls(), ring.started, ring.answered, ring.declined,
        ring.cancelled, ring.missed, ring.ended, ring.orphaned, ring.busy, ring.graced,
        ring.notices, bell.pushed, bell.unheard, bell.malformed, router.notices_forwarded,
        router.notices_fanned_out, router.notices_heard, router.notices_dropped);
}

} // namespace chat
