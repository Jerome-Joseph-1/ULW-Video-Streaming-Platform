#include "chat_service.hpp"

#include <algorithm>
#include <iterator>
#include <string>

namespace chat {

namespace {

// What a send holds while it waits, and a kept message while it is kept, besides its body:
// sender, key, seq and the container's own share.
constexpr std::size_t kMessageOverhead = 256;
// Rooms are looked over for lingering this often; a room lingers a second longer at most.
constexpr core::Millis kSweepEvery{1'000};

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

ChatService::ChatService(IRooms& rooms, core::ports::IMessageStore& messages,
                         const core::ports::IClock& clock, ServiceLimits limits)
    : rooms_plane_(rooms), messages_(messages), clock_(clock), limits_(limits),
      next_sweep_(clock.now()) {}

ChatService::~ChatService() {
    for (auto& [id, room] : rooms_) {
        if (room->joined || room->joining) {
            rooms_plane_.leave(id, *room);
        }
    }
}

ClientId ChatService::attach(IClient& client, const core::UserId& user) {
    const ClientId id{next_client_++};
    clients_.emplace(id.value, Client{.client = &client,
                                      .user = user,
                                      .rooms = {},
                                      .admitting = {},
                                      .send_bytes_in_flight = 0,
                                      .replayed_bytes = 0,
                                      .replay_window_start = clock_.now()});
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
    // Joining a room the client is in again is how it asks for what it missed. That costs a
    // join when it asks for a resume, which is work for this node; otherwise it is free.
    const bool known = std::ranges::find(c->rooms, join.room) != c->rooms.end();
    // Asked again before the member list answered: the first ask answers both.
    if (std::ranges::find(c->admitting, join.room) != c->admitting.end()) {
        answer(*c->client, "busy", join.room);
        return;
    }
    if (known && join.after && !admit_join(c->user)) {
        answer(*c->client, "busy", join.room);
        return;
    }
    if (!known) {
        if (c->rooms.size() >= limits_.max_rooms_per_client) {
            answer(*c->client, "too_many_rooms", join.room);
            return;
        }
        if (!admit_join(c->user)) {
            answer(*c->client, "busy", join.room);
            return;
        }
        // Whether the user may be in the room at all. Asked only for rooms new to the
        // connection, whose joins are rate limited. The room counts against the connection
        // while the answer is on its way, which comes on a later iteration, never from inside
        // the call; a call that could not be made leaves nothing behind.
        c->rooms.push_back(join.room);
        c->admitting.push_back(join.room);
        try {
            messages_.admits(join.room, c->user, join.kind,
                             [this, id, join](core::ports::MessageResult<bool> result) noexcept {
                                 admitted(id, join, result);
                             });
        } catch (...) {
            std::erase(c->admitting, join.room);
            std::erase(c->rooms, join.room);
            throw;
        }
        return;
    }
    enter(id, join);
}

void ChatService::admitted(ClientId id, const Join& join,
                           core::ports::MessageResult<bool> result) noexcept {
    Client* c = find(id);
    if (c == nullptr) {
        return;
    }
    std::erase(c->admitting, join.room);
    if (!result || !*result) {
        std::erase(c->rooms, join.room);
        answer(*c->client, result ? "not_member" : "unavailable", join.room);
        return;
    }
    try {
        enter(id, join);
    } catch (const std::bad_alloc&) {
        ++counters_.allocation_failures;
        std::erase(c->rooms, join.room);
        c->client->allocation_failed();
    }
}

// Into a room the client may be in: joined on the room plane once for the node, and subscribed.
void ChatService::enter(ClientId id, const Join& join) {
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
        replay(room, *c, *join.after);
    }
}

// The newest kept messages after `after` that fit the budget, sent oldest first. Anything older
// is a gap the client sees in the seqs, and fills from history. The budget is the client's for
// every resume within one linger, so that joining again and again replays no more.
void ChatService::replay(const Room& room, Client& c, std::uint64_t after) {
    const core::MonoTime now = clock_.now();
    if (now - c.replay_window_start >= limits_.linger) {
        c.replay_window_start = now;
        c.replayed_bytes = 0;
    }
    const std::size_t taken = std::max(c.client->unsent_bytes(), c.replayed_bytes);
    const std::size_t budget = limits_.replay_budget - std::min(taken, limits_.replay_budget);
    std::size_t used = 0;
    auto first = room.kept.end();
    while (first != room.kept.begin()) {
        const auto previous = std::prev(first);
        const std::size_t cost = message_wire_size(previous->body.size());
        if (previous->seq <= after || used + cost > budget) {
            break;
        }
        used += cost;
        first = previous;
    }
    c.replayed_bytes += used;
    std::string out;
    for (auto it = first; it != room.kept.end(); ++it) {
        out.clear();
        write_message(out, rt::Message{.room = room.id,
                                       .seq = it->seq,
                                       .sender = it->sender,
                                       .key = it->key,
                                       .body = it->body});
        c.client->push(out);
        ++counters_.replayed;
    }
}

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
    // Busy before the bucket: a send turned away for load costs the client no token.
    const std::size_t bytes = send.body.size() + kMessageOverhead;
    if (c->send_bytes_in_flight + bytes > limits_.max_send_bytes_in_flight) {
        answer(*c->client, reason(rt::RouteError::Busy), send.room, send.id);
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
    c->send_bytes_in_flight += bytes;
    rooms_plane_.send(send.room, *r, c->user, send.id, std::move(send.body),
                      [this, id, room = send.room, key = send.id,
                       bytes](std::expected<std::uint64_t, rt::RouteError> result) noexcept {
                          sent(id, room, key, bytes, result);
                      });
}

void ChatService::history(ClientId id, const History& history) {
    Client* c = find(id);
    if (c == nullptr) {
        return;
    }
    Room* r = find(history.room);
    if (r == nullptr || !r->joined ||
        std::ranges::find(r->subscribers, id, &Room::Subscriber::id) == r->subscribers.end()) {
        answer(*c->client, reason(rt::RouteError::NotJoined), history.room);
        return;
    }
    // A read of the store, and up to a resume's worth of output: charged as a resume is.
    if (!admit_join(c->user)) {
        answer(*c->client, "busy", history.room);
        return;
    }
    auto done =
        [this, id, room = history.room](
            core::ports::MessageResult<std::vector<core::ports::StoredMessage>> page) noexcept {
            page_read(id, room, std::move(page));
        };
    if (history.after) {
        messages_.history_after(history.room, *history.after, history.limit, std::move(done));
    } else {
        messages_.history_before(history.room, history.before, history.limit, std::move(done));
    }
}

// The page's messages in the store's order, as many as fit what the client may still queue,
// then the count. A client too far behind for even one gets busy, not an empty page, which
// would read as the end of the room.
void ChatService::page_read(
    ClientId id, const core::RoomId& room,
    core::ports::MessageResult<std::vector<core::ports::StoredMessage>> page) noexcept {
    Client* c = find(id);
    if (c == nullptr) {
        return;
    }
    if (!page) {
        answer(*c->client, reason(rt::RouteError::Unavailable), room);
        return;
    }
    try {
        const std::size_t budget =
            limits_.replay_budget - std::min(c->client->unsent_bytes(), limits_.replay_budget);
        // Every stored key came from a message key; one that is not is a row nothing here
        // wrote, and the page is not sent at all rather than sent with a hole in it.
        if (!std::ranges::all_of(*page, [](const core::ports::StoredMessage& m) {
                return rt::MessageKey::parse(m.key).has_value();
            })) {
            answer(*c->client, reason(rt::RouteError::Unavailable), room);
            return;
        }
        std::size_t used = 0;
        std::size_t count = 0;
        std::string out;
        for (const core::ports::StoredMessage& m : *page) {
            const std::size_t cost = message_wire_size(m.body.size());
            const auto key = rt::MessageKey::parse(m.key);
            if (used + cost > budget || !key) {
                break;
            }
            used += cost;
            out.clear();
            write_message(
                out,
                rt::Message{
                    .room = room, .seq = m.seq, .sender = m.sender, .key = *key, .body = m.body});
            c->client->push(out);
            ++count;
        }
        if (count == 0 && !page->empty()) {
            answer(*c->client, reason(rt::RouteError::Busy), room);
            return;
        }
        counters_.history_messages += count;
        out.clear();
        write_history(out, room, count);
        c->client->push(out);
    } catch (const std::bad_alloc&) {
        ++counters_.allocation_failures;
        c->client->allocation_failed();
    }
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
        // The owner shed it for load: the client did not get to send it, and gets its token
        // back to send it again.
        if (result.error() == rt::RouteError::Busy) {
            if (const auto bucket = sends_.find(c->user); bucket != sends_.end()) {
                bucket->second.give_back();
            }
        }
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
        if (s.client->push(text)) {
            ++counters_.delivered;
        }
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
