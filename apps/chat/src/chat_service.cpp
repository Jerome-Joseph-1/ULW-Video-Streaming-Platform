#include "chat_service.hpp"

#include <algorithm>
#include <iterator>
#include <string>

namespace chat {

namespace {

// What a send holds while it waits, and a kept message while it is kept, besides its body:
// sender, key, seq and the container's own share.
constexpr std::size_t kMessageOverhead = 256;
// A message on the wire besides its body: {"type":"message", the room (45), the seq (27), the
// sender (at most 140), the id (at most 72), and "body":"" (11) come to 313 bytes, and the
// WebSocket header of a frame under 64 KiB to 4 more.
constexpr std::size_t kFrameOverhead = 320;
// Rooms are looked over for lingering this often; a room lingers a second longer at most.
constexpr core::Millis kSweepEvery{1'000};

// A body as it travels to the client: base64url without padding, four characters per three
// bytes, and a partial group's two or three.
constexpr std::size_t encoded_size(std::size_t body) noexcept {
    return ((body * 4) + 2) / 3;
}

} // namespace

struct ChatService::Room final : rt::IMember {
    struct Subscriber {
        ClientId id;
        IClient* client;
        Delivery delivery;
    };
    // Clients that asked to join while the room plane has not answered yet.
    struct Waiting {
        ClientId id;
        Join join;
    };
    struct Kept {
        std::uint64_t seq;
        core::UserId sender;
        rt::MessageKey key;
        std::vector<std::byte> body;
    };

    Room(ChatService& owner, const core::RoomId& room) noexcept : service(owner), id(room) {}

    void deliver(const rt::Message& message) noexcept override {
        service.delivered(*this, message);
    }

    ChatService& service;
    core::RoomId id;
    // In the room plane: joined, or asked and not answered yet.
    bool joined = false;
    bool joining = false;
    std::vector<Subscriber> subscribers;
    std::vector<Waiting> waiting;
    std::deque<Kept> kept;
    std::size_t kept_bytes = 0;
    // The latest seq known: the room plane's answer to the join, or a later delivery.
    std::uint64_t head = 0;
    // Since when no client here has been in the room.
    core::MonoTime unused_since;
};

ChatService::ChatService(IRooms& rooms, const core::ports::IClock& clock, ServiceLimits limits)
    : rooms_plane_(rooms), clock_(clock), limits_(limits), next_sweep_(clock.now()) {}

ChatService::~ChatService() {
    for (auto& [id, room] : rooms_) {
        if (room->joined || room->joining) {
            rooms_plane_.leave(id, *room);
        }
    }
}

ClientId ChatService::attach(IClient& client, const core::UserId& user) {
    const ClientId id{next_client_++};
    clients_.emplace(id.value, Client{.client = &client, .user = user, .rooms = {}});
    return id;
}

void ChatService::detach(ClientId id) noexcept {
    const auto it = clients_.find(id.value);
    if (it == clients_.end()) {
        return;
    }
    const core::MonoTime now = clock_.now();
    for (const core::RoomId& room : it->second.rooms) {
        Room* r = find(room);
        if (r == nullptr) {
            continue;
        }
        std::erase_if(r->subscribers, [id](const Room::Subscriber& s) { return s.id == id; });
        std::erase_if(r->waiting, [id](const Room::Waiting& w) { return w.id == id; });
        if (r->subscribers.empty() && r->waiting.empty()) {
            r->unused_since = now;
        }
    }
    clients_.erase(it);
}

ChatService::Client* ChatService::find(ClientId id) noexcept {
    const auto it = clients_.find(id.value);
    return it == clients_.end() ? nullptr : &it->second;
}

ChatService::Room* ChatService::find(const core::RoomId& room) noexcept {
    const auto it = rooms_.find(room);
    return it == rooms_.end() ? nullptr : it->second.get();
}

// ---- joining

bool ChatService::admit_join(const core::UserId& user) {
    const core::MonoTime now = clock_.now();
    const auto it =
        joins_.try_emplace(user, limits_.join_burst, limits_.joins_per_second, now).first;
    return it->second.take(now).has_value();
}

void ChatService::join(ClientId id, const Join& join) {
    Client* c = find(id);
    if (c == nullptr) {
        return;
    }
    // Joining a room the client is in again costs nothing: it is how a client asks for what
    // it missed.
    if (std::ranges::find(c->rooms, join.room) == c->rooms.end()) {
        if (c->rooms.size() >= limits_.max_rooms_per_client) {
            answer(*c->client, "too_many_rooms", join.room);
            return;
        }
        if (!admit_join(c->user)) {
            answer(*c->client, "busy", join.room);
            return;
        }
        c->rooms.push_back(join.room);
    }
    auto it = rooms_.find(join.room);
    if (it == rooms_.end()) {
        auto made = std::make_unique<Room>(*this, join.room);
        it = rooms_.emplace(join.room, std::move(made)).first;
    }
    Room& r = *it->second;
    if (r.joined) {
        subscribe(r, id, join);
        return;
    }
    std::erase_if(r.waiting, [id](const Room::Waiting& w) { return w.id == id; });
    r.waiting.push_back({.id = id, .join = join});
    if (r.joining) {
        return;
    }
    r.joining = true;
    rooms_plane_.join(
        join.room, r,
        [this, room = join.room](std::expected<std::uint64_t, rt::RouteError> result) noexcept {
            joined(room, result);
        });
}

void ChatService::joined(const core::RoomId& room,
                         std::expected<std::uint64_t, rt::RouteError> result) noexcept {
    Room* r = find(room);
    if (r == nullptr) {
        return;
    }
    r->joining = false;
    const std::vector<Room::Waiting> waiting = std::move(r->waiting);
    r->waiting.clear();
    if (!result) {
        for (const Room::Waiting& w : waiting) {
            if (Client* c = find(w.id)) {
                std::erase(c->rooms, room);
                answer(*c->client, reason(result.error()), room);
            }
        }
        if (r->subscribers.empty()) {
            erase(room);
        }
        return;
    }
    r->joined = true;
    r->head = std::max(r->head, *result);
    r->unused_since = clock_.now();
    for (const Room::Waiting& w : waiting) {
        Client* c = find(w.id);
        if (c == nullptr) {
            continue;
        }
        try {
            subscribe(*r, w.id, w.join);
        } catch (const std::bad_alloc&) {
            ++counters_.allocation_failures;
            c->client->allocation_failed();
        }
    }
}

void ChatService::subscribe(Room& room, ClientId id, const Join& join) {
    Client* c = find(id);
    if (c == nullptr) {
        return;
    }
    const auto it = std::ranges::find(room.subscribers, id, &Room::Subscriber::id);
    if (it == room.subscribers.end()) {
        room.subscribers.push_back({.id = id, .client = c->client, .delivery = join.delivery});
    } else {
        it->delivery = join.delivery;
    }
    std::string out;
    write_joined(out, room.id, room.head);
    c->client->push(out);
    if (join.after) {
        replay(room, *c->client, *join.after);
    }
}

// The newest kept messages after `after` that fit the budget, sent oldest first. Anything older
// is a gap the client sees in the seqs, and fills from history.
void ChatService::replay(const Room& room, IClient& client, std::uint64_t after) {
    const std::size_t budget =
        limits_.replay_budget - std::min(client.unsent_bytes(), limits_.replay_budget);
    std::size_t used = 0;
    auto first = room.kept.end();
    while (first != room.kept.begin()) {
        const auto previous = std::prev(first);
        const std::size_t cost = encoded_size(previous->body.size()) + kFrameOverhead;
        if (previous->seq <= after || used + cost > budget) {
            break;
        }
        used += cost;
        first = previous;
    }
    std::string out;
    for (auto it = first; it != room.kept.end(); ++it) {
        out.clear();
        write_message(out, rt::Message{.room = room.id,
                                       .seq = it->seq,
                                       .sender = it->sender,
                                       .key = it->key,
                                       .body = it->body});
        client.push(out);
        ++counters_.replayed;
    }
}

// ---- sending

void ChatService::send(ClientId id, Send send) {
    Client* c = find(id);
    if (c == nullptr) {
        return;
    }
    Room* r = find(send.room);
    if (r == nullptr || !r->joined ||
        std::ranges::find(r->subscribers, id, &Room::Subscriber::id) == r->subscribers.end()) {
        answer(*c->client, reason(rt::RouteError::NotJoined), send.room, send.id);
        return;
    }
    const core::MonoTime now = clock_.now();
    const auto bucket =
        sends_.try_emplace(c->user, limits_.send_burst, limits_.sends_per_second, now).first;
    if (const auto taken = bucket->second.take(now); !taken) {
        ++counters_.rate_limited;
        std::string out;
        write_rate_limited(out, send.room, send.id, taken.error());
        c->client->push(out);
        return;
    }
    const std::size_t bytes = send.body.size() + kMessageOverhead;
    if (c->send_bytes_in_flight + bytes > limits_.max_send_bytes_in_flight) {
        answer(*c->client, reason(rt::RouteError::Busy), send.room, send.id);
        return;
    }
    c->send_bytes_in_flight += bytes;
    rooms_plane_.send(send.room, *r, c->user, send.id, std::move(send.body),
                      [this, id, room = send.room, key = send.id,
                       bytes](std::expected<std::uint64_t, rt::RouteError> result) noexcept {
                          sent(id, room, key, bytes, result);
                      });
}

void ChatService::sent(ClientId id, const core::RoomId& room, const rt::MessageKey& key,
                       std::size_t bytes,
                       std::expected<std::uint64_t, rt::RouteError> result) noexcept {
    Client* c = find(id);
    if (c == nullptr) {
        return;
    }
    c->send_bytes_in_flight -= bytes;
    if (!result) {
        answer(*c->client, reason(result.error()), room, key);
        return;
    }
    try {
        std::string out;
        write_sent(out, room, key, *result);
        c->client->push(out);
    } catch (const std::bad_alloc&) {
        ++counters_.allocation_failures;
        c->client->allocation_failed();
    }
}

// ---- delivering

// One text for every client in the room. A client that cannot be given it (the text could not
// be made) is closed if it asked never to miss a message; it resumes when it reconnects.
void ChatService::delivered(Room& room, const rt::Message& message) noexcept {
    std::string text;
    try {
        write_message(text, message);
    } catch (const std::bad_alloc&) {
        ++counters_.allocation_failures;
        for (const Room::Subscriber& s : room.subscribers) {
            if (s.delivery == Delivery::Durable) {
                s.client->allocation_failed();
            }
        }
        return;
    }
    room.head = std::max(room.head, message.seq);
    try {
        keep(room, message);
    } catch (const std::bad_alloc&) {
        // Only a client resuming later misses it, and sees the gap.
        ++counters_.allocation_failures;
    }
    for (const Room::Subscriber& s : room.subscribers) {
        if (s.delivery == Delivery::Lossy && s.client->unsent_bytes() > limits_.lossy_backlog) {
            ++counters_.lossy_drops;
            continue;
        }
        s.client->push(text);
        ++counters_.delivered;
    }
}

void ChatService::keep(Room& room, const rt::Message& message) {
    const std::size_t cost = message.body.size() + kMessageOverhead;
    // The order is recorded first: an entry there whose message is not kept is skipped later,
    // while a kept message missing from it would never be dropped.
    kept_order_.emplace_back(room.id, message.seq);
    room.kept.push_back({.seq = message.seq,
                         .sender = message.sender,
                         .key = message.key,
                         .body = {message.body.begin(), message.body.end()}});
    room.kept_bytes += cost;
    buffered_bytes_ += cost;
    while (room.kept_bytes > limits_.room_buffer_bytes) {
        drop_oldest(room);
    }
    while (!kept_order_.empty() && (buffered_bytes_ > limits_.buffer_bytes ||
                                    kept_order_.size() > limits_.buffer_messages)) {
        forget_oldest();
    }
}

void ChatService::drop_oldest(Room& room) noexcept {
    const std::size_t cost = room.kept.front().body.size() + kMessageOverhead;
    room.kept_bytes -= cost;
    buffered_bytes_ -= cost;
    room.kept.pop_front();
}

// A room's messages are kept in seq order, and in the same order here, so the oldest kept
// anywhere is its room's first unless that one is gone already.
void ChatService::forget_oldest() noexcept {
    const auto [room, seq] = kept_order_.front();
    kept_order_.pop_front();
    Room* r = find(room);
    if (r != nullptr && !r->kept.empty() && r->kept.front().seq == seq) {
        drop_oldest(*r);
    }
}

// ---- lifetime

void ChatService::sweep() noexcept {
    const core::MonoTime now = clock_.now();
    if (now < next_sweep_) {
        return;
    }
    next_sweep_ = now + kSweepEvery;
    std::vector<core::RoomId> unused;
    try {
        for (const auto& [id, room] : rooms_) {
            if (room->joined && room->subscribers.empty() && room->waiting.empty() &&
                now - room->unused_since >= limits_.linger) {
                unused.push_back(id);
            }
        }
    } catch (const std::bad_alloc&) {
        // Those found so far go now, the rest at a later sweep.
        ++counters_.allocation_failures;
    }
    for (const core::RoomId& room : unused) {
        erase(room);
    }
    // A full bucket is the same as none.
    std::erase_if(joins_, [now](const auto& entry) { return entry.second.full(now); });
    std::erase_if(sends_, [now](const auto& entry) { return entry.second.full(now); });
}

void ChatService::erase(const core::RoomId& room) noexcept {
    const auto it = rooms_.find(room);
    if (it == rooms_.end()) {
        return;
    }
    Room& r = *it->second;
    if (r.joined || r.joining) {
        rooms_plane_.leave(room, r);
    }
    buffered_bytes_ -= r.kept_bytes;
    rooms_.erase(it);
}

void ChatService::answer(IClient& client, std::string_view reason, const core::RoomId& room,
                         const std::optional<rt::MessageKey>& id) noexcept {
    try {
        std::string out;
        write_error(out, reason, room, id);
        client.push(out);
    } catch (const std::bad_alloc&) {
        ++counters_.allocation_failures;
        client.allocation_failed();
    }
}

} // namespace chat
