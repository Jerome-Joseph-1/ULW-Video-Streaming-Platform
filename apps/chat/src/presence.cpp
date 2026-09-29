#include "presence.hpp"

#include "envelope.hpp"
#include "presence_room.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <iterator>
#include <span>
#include <string>

namespace chat {

namespace {

// A send the room plane refused, or a join it could not make, is tried again this much later:
// soon enough that a grace is late by little, and slow enough that an owner being replaced is
// not hammered.
constexpr core::Millis kRetry{1'000};
// Deadlines (graces, renewals, expiries) are looked for this often, so each is at most this
// late; a second is nothing next to a grace of ten.
constexpr core::Millis kScanEvery{1'000};
// Nodes a room remembers as watching, or as announcing its user. A deployment runs three
// replicas and the router takes 32 peers at most (ADR-0035); restarted nodes' earlier runs stay
// until their announcements expire. Past it, an event from yet another node is ignored.
constexpr std::size_t kMaxNodesPerRoom = 64;
// A kind byte, then the sender's tag in 8 bytes, big-endian.
constexpr std::size_t kEventSize = 9;

std::uint64_t incarnation_tag(const core::NodeId& self, core::WallTime started) {
    const auto digest =
        sha256(std::format("{} {}", self.view(), started.time_since_epoch().count()));
    std::uint64_t tag = 0;
    for (const unsigned char b : std::span(digest).first<sizeof tag>()) {
        tag = (tag << 8U) | b;
    }
    return tag;
}

} // namespace

enum class Presence::Kind : std::uint8_t {
    Hello = 1,
    Unwatch = 2,
    Probe = 3,
    Ack = 4,
    Online = 5,
    Offline = 6,
};

std::optional<Presence::Kind> Presence::kind_of(std::byte b) noexcept {
    const auto value = std::to_integer<std::uint8_t>(b);
    if (value < static_cast<std::uint8_t>(Kind::Hello) ||
        value > static_cast<std::uint8_t>(Kind::Offline)) {
        return std::nullopt;
    }
    return static_cast<Kind>(value);
}

struct Presence::Room final : rt::IMember {
    // Another node, as last heard from.
    struct Heard {
        std::uint64_t node;
        core::MonoTime at;
    };

    Room(Presence& owner, const core::UserId& watched)
        : service(owner), user(watched), id(presence_room(watched)) {}

    void deliver(const rt::Message& message) noexcept override {
        service.delivered(*this, message);
    }

    [[nodiscard]] bool wanted() const noexcept { return connections > 0 || grace_ends; }
    [[nodiscard]] bool online() const noexcept { return wanted() || !sources.empty(); }

    Presence& service;
    core::UserId user;
    core::RoomId id;
    bool joined = false;
    bool joining = false;
    // The latest seq seen: the join's answer (the room's last_seq) or a delivery. 0 at the join
    // means nothing was ever said in the room, so no node was watching then; one that starts
    // later says hello, which this node hears.
    std::uint64_t head = 0;
    bool sending = false;
    std::optional<core::MonoTime> retry_at;
    bool dirty = false;

    // The user's side: their connections on this node, and what this node has said of them.
    std::size_t connections = 0;
    std::optional<core::MonoTime> grace_ends;
    // An online or probe of this node's stands in the room.
    bool announced = false;
    // A hello, or a failed announcement, wants an online from this node.
    bool answer = false;
    core::MonoTime refresh_at;
    std::vector<Heard> watchers;

    // The watchers' side: clients here that watch the user, and every node's announcement.
    std::vector<PresenceClientId> local;
    // This node's hello stands in the room.
    bool watching = false;
    bool ack = false;
    std::vector<Heard> sources;
    bool shown_online = false;
};

Presence::Presence(IRooms& rooms, net::IReactor& reactor, const core::ports::IClock& clock,
                   const core::NodeId& self, PresenceLimits limits)
    : rooms_plane_(rooms), reactor_(reactor), clock_(clock), limits_(limits),
      tag_(incarnation_tag(self, clock.wall_now())), next_scan_(clock.now()) {}

Presence::~Presence() {
    reactor_.cancel_timer(timer_);
    for (auto& [user, room] : rooms_) {
        if (room->joined || room->joining) {
            rooms_plane_.leave(room->id, *room);
        }
    }
}

Presence::Room& Presence::room_of(const core::UserId& user) {
    auto it = rooms_.find(user);
    if (it == rooms_.end()) {
        auto made = std::make_unique<Room>(*this, user);
        it = rooms_.emplace(user, std::move(made)).first;
    }
    return *it->second;
}

PresenceClientId Presence::attach(IClient& client, const core::UserId& user) {
    Room& r = room_of(user);
    const PresenceClientId id{next_client_++};
    clients_.emplace(id.value, Client{.client = &client, .user = user, .watching = {}});
    ++r.connections;
    // Back within the grace: as far as anyone else can tell, the user never left.
    r.grace_ends.reset();
    show(r);
    wake(r);
    return id;
}

void Presence::detach(PresenceClientId id) noexcept {
    const auto it = clients_.find(id.value);
    if (it == clients_.end()) {
        return;
    }
    for (Room* room : it->second.watching) {
        drop_watch(*room, id);
    }
    if (const auto r = rooms_.find(it->second.user); r != rooms_.end()) {
        Room& room = *r->second;
        if (--room.connections == 0) {
            room.grace_ends = clock_.now() + limits_.grace;
            arm(*room.grace_ends);
        }
    }
    clients_.erase(it);
}

void Presence::watch(PresenceClientId id, const core::UserId& user) {
    const auto c = clients_.find(id.value);
    if (c == clients_.end()) {
        return;
    }
    Client& client = c->second;
    const auto known = rooms_.find(user);
    Room* room = known == rooms_.end() ? nullptr : known->second.get();
    if (room != nullptr && std::ranges::find(client.watching, room) != client.watching.end()) {
        tell(*client.client, "watching", *room);
        return;
    }
    std::string refusal;
    if (user == client.user) {
        // The connection asking is itself the answer; watching it would only cost events.
        write_user_error(refusal, "watching_self", user);
    } else if (client.watching.size() >= limits_.max_watches_per_client) {
        write_user_error(refusal, "too_many_watches", user);
    } else if (room == nullptr && rooms_.size() >= limits_.max_rooms) {
        write_user_error(refusal, "busy", user);
    } else if (room == nullptr || room->local.empty()) {
        // This node starts watching the user: a hello now, an unwatch later.
        const core::MonoTime now = clock_.now();
        const auto bucket =
            watch_joins_
                .try_emplace(client.user, limits_.watch_burst, limits_.watches_per_second, now)
                .first;
        if (!bucket->second.take(now)) {
            write_user_error(refusal, "busy", user);
        }
    }
    if (!refusal.empty()) {
        client.client->push(refusal);
        return;
    }
    Room& r = room_of(user);
    // The room first: a room with a local watcher is never erased, so the pointer the client
    // keeps is never left dangling.
    r.local.push_back(id);
    try {
        client.watching.push_back(&r);
    } catch (...) {
        r.local.pop_back();
        throw;
    }
    tell(*client.client, "watching", r);
    wake(r);
}

void Presence::unwatch(PresenceClientId id, const core::UserId& user) {
    const auto c = clients_.find(id.value);
    const auto r = rooms_.find(user);
    if (c == clients_.end() || r == rooms_.end() ||
        std::erase(c->second.watching, r->second.get()) == 0) {
        return;
    }
    drop_watch(*r->second, id);
}

void Presence::drop_watch(Room& room, PresenceClientId id) noexcept {
    std::erase(room.local, id);
    wake(room);
}

// Events are idempotent: an online from a node already announcing, an offline from one that
// is not, a second hello or ack, change nothing. That is what lets every send be retried
// without a key of its own, and a node act on the room's order alone.
void Presence::delivered(Room& room, const rt::Message& message) noexcept {
    // Seqs rise by one; a jump is events this node never got, among them perhaps a hello it
    // owed an answer or an announcement it should know of. Nobody can tell whom a gap cost, so
    // this node says again what it stands for, and whoever it cost does likewise.
    // An announcement is repeated as a probe, not an online: the gap may have held acks, and a
    // watcher this node stops hearing from is one it stops renewing for.
    if (message.seq > room.head + 1) {
        ++counters_.gaps;
        room.watching = room.watching && room.local.empty();
        room.announced = room.announced && !room.wanted();
    }
    room.head = std::max(room.head, message.seq);
    // Only nodes speak in a presence room, each under the user's name; clients cannot reach
    // it (the envelope refuses version 8 room ids).
    if (message.sender != room.user || message.body.size() != kEventSize) {
        return;
    }
    const auto kind = kind_of(message.body[0]);
    if (!kind) {
        return;
    }
    std::uint64_t node = 0;
    for (std::size_t i = 1; i < kEventSize; ++i) {
        node = (node << 8U) | std::to_integer<std::uint64_t>(message.body[i]);
    }
    ++counters_.received;
    const core::MonoTime now = clock_.now();
    const auto remember = [now](std::vector<Room::Heard>& nodes, std::uint64_t n) {
        const auto it = std::ranges::find(nodes, n, &Room::Heard::node);
        if (it != nodes.end()) {
            it->at = now;
        } else if (nodes.size() < kMaxNodesPerRoom) {
            nodes.push_back({.node = n, .at = now});
        }
    };
    const auto forget = [](std::vector<Room::Heard>& nodes, std::uint64_t n) {
        std::erase_if(nodes, [n](const Room::Heard& h) { return h.node == n; });
    };
    try {
        switch (*kind) {
        case Kind::Hello:
            remember(room.watchers, node);
            room.answer = room.answer || room.wanted();
            break;
        case Kind::Ack:
            remember(room.watchers, node);
            break;
        case Kind::Unwatch:
            forget(room.watchers, node);
            break;
        case Kind::Probe:
        case Kind::Online:
            remember(room.sources, node);
            room.ack = room.ack || (*kind == Kind::Probe && node != tag_ && !room.local.empty());
            break;
        case Kind::Offline:
            forget(room.sources, node);
            break;
        }
    } catch (const std::bad_alloc&) {
        // A watcher or a source this node does not learn of: a renewal, or the next hello or
        // probe, tells it again.
        ++counters_.allocation_failures;
    }
    show(room);
    wake(room);
}

void Presence::joined(const core::UserId& user,
                      std::expected<std::uint64_t, rt::RouteError> result) noexcept {
    const auto it = rooms_.find(user);
    if (it == rooms_.end()) {
        return;
    }
    Room& r = *it->second;
    r.joining = false;
    if (!result) {
        r.retry_at = clock_.now() + kRetry;
        arm(*r.retry_at);
        return;
    }
    r.joined = true;
    r.head = std::max(r.head, *result);
    wake(r);
}

// What this node owes the room, one event at a time so that the room's order is the order it
// was decided in. The user's side goes first: an announcement is what a watcher is waiting for.
void Presence::pump(Room& room, core::MonoTime now) {
    if (room.retry_at && now < *room.retry_at) {
        return;
    }
    room.retry_at.reset();
    if (!room.joined) {
        if (!room.joining && !idle(room)) {
            // Set first: the answer may come inside the call.
            room.joining = true;
            try {
                rooms_plane_.join(room.id, room,
                                  [this, user = room.user](
                                      std::expected<std::uint64_t, rt::RouteError> r) noexcept {
                                      joined(user, r);
                                  });
            } catch (...) {
                room.joining = false;
                throw;
            }
        }
        return;
    }
    if (room.sending) {
        return;
    }
    const bool wanted = room.wanted();
    const bool watched_elsewhere =
        std::ranges::any_of(room.watchers, [this](const Room::Heard& h) { return h.node != tag_; });
    const bool renew = room.announced && now >= room.refresh_at && watched_elsewhere;
    if (wanted && ((!room.announced && (room.head > 0 || room.answer)) || renew)) {
        // A first announcement where someone may have been watching, or a renewal: either way
        // the watching nodes answer, so that this node knows who still watches.
        post(room, Kind::Probe, now);
    } else if (!wanted && room.announced) {
        post(room, Kind::Offline, now);
    } else if (wanted && room.answer) {
        post(room, Kind::Online, now);
    } else if (room.local.empty() == room.watching) {
        post(room, room.watching ? Kind::Unwatch : Kind::Hello, now);
    } else if (room.ack && room.watching) {
        post(room, Kind::Ack, now);
    }
}

void Presence::post(Room& room, Kind kind, core::MonoTime now) {
    // Unique per run of this node, which is all the room plane's retry detection needs; the
    // events themselves are idempotent.
    const auto key = rt::MessageKey::parse(std::format("{:016x}-{:x}", tag_, next_key_++));
    if (!key) {
        return;
    }
    std::vector<std::byte> body(kEventSize);
    body[0] = static_cast<std::byte>(kind);
    for (std::size_t i = 1; i < kEventSize; ++i) {
        body[i] = static_cast<std::byte>(tag_ >> (8U * (kEventSize - 1 - i)));
    }
    // What the send stands for is set before it, since its answer may come inside the call,
    // and put back if the call throws, as though it had never been made.
    struct Before {
        bool announced = false;
        bool answer = false;
        core::MonoTime refresh_at;
        bool watching = false;
        bool ack = false;
    };
    const Before before{.announced = room.announced,
                        .answer = room.answer,
                        .refresh_at = room.refresh_at,
                        .watching = room.watching,
                        .ack = room.ack};
    switch (kind) {
    case Kind::Probe:
    case Kind::Online:
        room.announced = true;
        room.answer = false;
        room.refresh_at = now + limits_.refresh;
        break;
    case Kind::Offline:
        room.announced = false;
        break;
    case Kind::Hello:
        room.watching = true;
        break;
    case Kind::Unwatch:
        room.watching = false;
        break;
    case Kind::Ack:
        room.ack = false;
        break;
    }
    room.sending = true;
    try {
        rooms_plane_.send(room.id, room, room.user, *key, std::move(body),
                          [this, user = room.user,
                           kind](std::expected<std::uint64_t, rt::RouteError> result) noexcept {
                              posted(user, kind, result);
                          });
    } catch (...) {
        room.announced = before.announced;
        room.answer = before.answer;
        room.refresh_at = before.refresh_at;
        room.watching = before.watching;
        room.ack = before.ack;
        room.sending = false;
        throw;
    }
    ++counters_.sent;
}

// A failed send may or may not have been sequenced. Either way it is sent again if the state
// that called for it still holds; the events are idempotent.
void Presence::posted(const core::UserId& user, Kind kind,
                      std::expected<std::uint64_t, rt::RouteError> result) noexcept {
    const auto it = rooms_.find(user);
    if (it == rooms_.end()) {
        return;
    }
    Room& r = *it->second;
    r.sending = false;
    if (!result) {
        switch (kind) {
        case Kind::Probe:
        case Kind::Online:
            r.answer = r.wanted();
            break;
        case Kind::Offline:
            r.announced = !r.wanted();
            break;
        case Kind::Hello:
            r.watching = false;
            break;
        case Kind::Unwatch:
            r.watching = true;
            break;
        case Kind::Ack:
            r.ack = true;
            break;
        }
        r.retry_at = clock_.now() + kRetry;
        arm(*r.retry_at);
        return;
    }
    wake(r);
}

void Presence::expire(Room& room, core::MonoTime now) noexcept {
    if (room.grace_ends && now >= *room.grace_ends && room.connections == 0) {
        room.grace_ends.reset();
    }
    // This node's own announcement and watch are known here first-hand; only other nodes'
    // run out.
    const auto stale = [&](const Room::Heard& h) {
        return h.node != tag_ && now - h.at >= limits_.expiry;
    };
    const auto lost_sources = std::erase_if(room.sources, stale);
    counters_.expired += lost_sources + std::erase_if(room.watchers, stale);
    // An announcement that ran out may only have lost its renewals: saying hello again is
    // answered by every node that still has the user.
    if (lost_sources > 0 && !room.local.empty()) {
        room.watching = false;
    }
    show(room);
}

void Presence::show(Room& room) noexcept {
    const bool online = room.online();
    if (online == room.shown_online) {
        return;
    }
    room.shown_online = online;
    for (const PresenceClientId id : room.local) {
        if (const auto c = clients_.find(id.value); c != clients_.end()) {
            tell(*c->second.client, "presence", room);
            ++counters_.notified;
        }
    }
}

void Presence::tell(IClient& client, std::string_view type, const Room& room) noexcept {
    try {
        std::string out;
        write_presence(out, type, room.user, room.shown_online);
        client.push(out);
    } catch (const std::bad_alloc&) {
        ++counters_.allocation_failures;
        client.allocation_failed();
    }
}

bool Presence::idle(const Room& room) noexcept {
    return !room.wanted() && room.local.empty() && !room.announced && !room.watching &&
           !room.sending;
}

void Presence::wake(Room& room) noexcept {
    if (room.dirty) {
        return;
    }
    try {
        dirty_.push_back(room.user);
    } catch (const std::bad_alloc&) {
        // The scan finds it within kScanEvery.
        ++counters_.allocation_failures;
        return;
    }
    room.dirty = true;
    arm(clock_.now());
}

void Presence::arm(core::MonoTime at) noexcept {
    if (armed_at_ && *armed_at_ <= at) {
        return;
    }
    reactor_.cancel_timer(timer_);
    const core::MonoTime now = clock_.now();
    const auto delay = at > now ? std::chrono::ceil<core::Millis>(at - now) : core::Millis{0};
    timer_ = reactor_.arm_timer(delay, *this);
    armed_at_ = at;
}

void Presence::on_timeout() noexcept {
    timer_ = {};
    armed_at_.reset();
    const core::MonoTime now = clock_.now();
    const std::vector<core::UserId> woken = std::move(dirty_);
    dirty_.clear();
    // Pumping never adds a room, and a callback it runs at once only wakes one, so the map
    // can be walked while visited rooms are erased from it.
    const auto visit = [&](auto it) {
        Room& r = *it->second;
        r.dirty = false;
        pump(r, now);
        if (!idle(r) || r.joining) {
            return std::next(it);
        }
        if (r.joined) {
            rooms_plane_.leave(r.id, r);
        }
        return rooms_.erase(it);
    };
    try {
        if (now >= next_scan_) {
            next_scan_ = now + kScanEvery;
            for (auto it = rooms_.begin(); it != rooms_.end();) {
                expire(*it->second, now);
                it = visit(it);
            }
            std::erase_if(watch_joins_,
                          [now](const auto& entry) { return entry.second.full(now); });
        } else {
            for (const core::UserId& user : woken) {
                if (const auto it = rooms_.find(user); it != rooms_.end()) {
                    visit(it);
                }
            }
        }
    } catch (const std::bad_alloc&) {
        // A room not visited keeps its flag, and the next scan visits every room.
        ++counters_.allocation_failures;
    }
    if (!dirty_.empty()) {
        arm(now);
    } else if (!rooms_.empty()) {
        arm(next_scan_);
    }
}

} // namespace chat
