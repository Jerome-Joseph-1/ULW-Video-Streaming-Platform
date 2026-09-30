#include "chat_service.hpp"

#include <algorithm>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace chat {

namespace {

// What a send holds while it waits, and a kept message while it is kept, besides its body:
// sender, key, seq and the container's own share.
constexpr std::size_t kMessageOverhead = 256;
// Rooms are looked over for lingering this often; a room lingers a second longer at most.
constexpr core::Millis kSweepEvery{1'000};
// A resync's member checks asked at once: the message store's pool (MessageStoreConfig), so
// that joins and history reads queued behind them wait for at most one check each.
constexpr std::size_t kRechecksInFlight = 4;
// Checks a resync may owe: one per room of each client, at 1280 connections of 64 rooms.
constexpr std::size_t kMaxRechecks = std::size_t{1280} * 64;

// What a lossy client behind from `behind` is still owed of a room whose latest seq is `head`:
// dropped for it when it leaves or joins again before it is sent them.
[[nodiscard]] std::uint64_t owed(std::optional<std::uint64_t> behind, std::uint64_t head) noexcept {
    return behind && head >= *behind ? head - *behind + 1 : 0;
}

// The error a join is refused with, for an answer other than Admitted.
[[nodiscard]] std::string_view
refusal(const core::ports::MessageResult<core::ports::Admission>& result) noexcept {
    if (!result) {
        return "unavailable";
    }
    return *result == core::ports::Admission::NotLive ? "not_live" : "not_member";
}

} // namespace

struct ChatService::Room final : rt::IMember {
    struct Subscriber {
        ClientId id;
        IClient* client;
        Delivery delivery;
        // A lossy client that fell behind: the first seq it has not been sent. Nothing new
        // reaches it until its connection drains; then it is sent the kept messages from
        // there on.
        std::optional<std::uint64_t> behind;
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
    // A stream's live chat: what this node lets into it, from all its senders together.
    std::optional<TokenBucket> live_sends;
    // The latest seq known: the room plane's answer to the join, or a later delivery.
    std::uint64_t head = 0;
    // Since when no client here has been in the room.
    core::MonoTime unused_since;
};

ChatService::ChatService(IRooms& rooms, core::ports::IMessageStore& messages,
                         const core::ports::IClock& clock, ServiceLimits limits)
    : rooms_plane_(rooms), messages_(messages), clock_(clock), limits_(limits),
      next_sweep_(clock.now()), rechecks_resume_(clock.now()) {
    messages_.watch_members(this);
}

// No watch_members(nullptr) here: the store is gone by now (destroyed first, as the
// constructor's contract says), and took the listener with it.
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
                                      .revoked = {},
                                      .unchecked = {},
                                      .behind = {},
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
        if (const auto s = std::ranges::find(r->subscribers, id, &Room::Subscriber::id);
            s != r->subscribers.end()) {
            counters_.lossy_drops += owed(s->behind, r->head);
            r->subscribers.erase(s);
        }
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
            messages_.admits(
                join.room, c->user, join.kind,
                [this, id,
                 join](core::ports::MessageResult<core::ports::Admission> result) noexcept {
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
                           core::ports::MessageResult<core::ports::Admission> result) noexcept {
    Client* c = find(id);
    if (c == nullptr) {
        return;
    }
    std::erase(c->admitting, join.room);
    if (std::erase(c->revoked, join.room) != 0) {
        result = core::ports::Admission::NotMember;
    }
    const bool unchecked = std::erase(c->unchecked, join.room) != 0;
    if (!result || *result != core::ports::Admission::Admitted) {
        std::erase(c->rooms, join.room);
        answer(*c->client, refusal(result), join.room);
        return;
    }
    try {
        enter(id, join);
    } catch (const std::bad_alloc&) {
        ++counters_.allocation_failures;
        std::erase(c->rooms, join.room);
        c->client->allocation_failed();
        return;
    }
    if (!unchecked) {
        return;
    }
    // Let in by a list that may have been read before a removal the resync was for.
    try {
        if (!recheck(join.room, c->user)) {
            resync_owed_ = true;
        }
    } catch (const std::bad_alloc&) {
        ++counters_.allocation_failures;
        resync_owed_ = true;
    }
    ask_rechecks();
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
    // An audience of thousands is not held back by its slowest viewer, nor closed for being
    // one: a live chat is lossy whatever the join asked for.
    const Delivery delivery =
        core::ports::is_stream_chat(room.id) ? Delivery::Lossy : join.delivery;
    const auto it = std::ranges::find(room.subscribers, id, &Room::Subscriber::id);
    std::optional<std::uint64_t> was_behind;
    if (it == room.subscribers.end()) {
        room.subscribers.push_back(
            {.id = id, .client = c->client, .delivery = delivery, .behind = std::nullopt});
    } else {
        // What it was owed as a lossy client is not sent now that it has joined again, beyond
        // what this join's resume sends it; the seqs it sees show the gap.
        it->delivery = delivery;
        was_behind = it->behind;
        it->behind.reset();
        std::erase(c->behind, room.id);
    }
    std::string out;
    write_joined(out, room.id, room.head);
    c->client->push(out);
    const std::uint64_t resumed_from = join.after ? replay(room, *c, *join.after) : room.head + 1;
    counters_.lossy_drops += owed(was_behind, std::min(room.head, resumed_from - 1));
}

// The newest kept messages after `after` that fit the budget, sent oldest first. Anything older
// is a gap the client sees in the seqs, and fills from history. The budget is the client's for
// every resume within one linger, so that joining again and again replays no more. Answers the
// first seq it sent, or the one past the room's head when it sent none.
std::uint64_t ChatService::replay(const Room& room, Client& c, std::uint64_t after) {
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
    return first == room.kept.end() ? room.head + 1 : first->seq;
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
    const bool live = core::ports::is_stream_chat(send.room);
    if (live && send.body.size() > limits_.live_body) {
        answer(*c->client, "too_large", send.room, send.id);
        return;
    }
    const core::MonoTime now = clock_.now();
    const auto bucket =
        sends_.try_emplace(c->user, limits_.send_burst, limits_.sends_per_second, now).first;
    auto taken = bucket->second.take(now);
    if (taken && live) {
        if (!r->live_sends) {
            r->live_sends.emplace(limits_.live_room_burst, limits_.live_room_sends_per_second, now);
        }
        // The room's allowance ran out, not the user's: the user keeps their token.
        taken = r->live_sends->take(now);
        if (!taken) {
            bucket->second.give_back();
        }
    }
    if (!taken) {
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
    // Gone from the room meanwhile, taken out by a removal from its member list.
    if (c == nullptr || std::ranges::find(c->rooms, room) == c->rooms.end()) {
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
    for (Room::Subscriber& s : room.subscribers) {
        if (s.delivery == Delivery::Lossy &&
            (s.behind || s.client->unsent_bytes() > limits_.lossy_backlog)) {
            if (!s.behind) {
                s.behind = message.seq;
                fell_behind(s.id, room.id);
            }
            // Owed the newest lossy_depth at most: the oldest beyond that are dropped.
            const std::uint64_t oldest_owed =
                message.seq >= limits_.lossy_depth ? message.seq - limits_.lossy_depth + 1 : 0;
            if (*s.behind < oldest_owed) {
                counters_.lossy_drops += oldest_owed - *s.behind;
                s.behind = oldest_owed;
            }
            continue;
        }
        if (s.client->push(text)) {
            ++counters_.delivered;
        }
    }
}

void ChatService::fell_behind(ClientId id, const core::RoomId& room) noexcept {
    Client* c = find(id);
    if (c == nullptr) {
        return;
    }
    try {
        c->behind.push_back(room);
    } catch (const std::bad_alloc&) {
        // Without the note it would never be sent what it is owed: it pays with its
        // connection, as for any delivery it could not be given.
        ++counters_.allocation_failures;
        c->client->allocation_failed();
    }
}

void ChatService::drained(ClientId id) noexcept {
    Client* c = find(id);
    if (c == nullptr || c->behind.empty()) {
        return;
    }
    // catch_up takes rooms off the list as the client catches up in them.
    const std::vector<core::RoomId> behind = std::exchange(c->behind, {});
    for (const core::RoomId& room : behind) {
        Room* r = find(room);
        if (r == nullptr) {
            continue;
        }
        try {
            catch_up(*r, id, *c);
        } catch (const std::bad_alloc&) {
            ++counters_.allocation_failures;
            c->client->allocation_failed();
            return;
        }
    }
}

// The kept messages from where the client fell behind, oldest first, for as long as its
// connection takes them. Anything it was owed that is no longer kept was counted as dropped
// when it went.
void ChatService::catch_up(Room& room, ClientId id, Client& c) {
    const auto s = std::ranges::find(room.subscribers, id, &Room::Subscriber::id);
    if (s == room.subscribers.end()) {
        return;
    }
    const std::optional<std::uint64_t> from = s->behind;
    if (!from) {
        return;
    }
    std::uint64_t next = *from;
    std::string out;
    for (auto it = std::ranges::lower_bound(room.kept, next, {}, &Room::Kept::seq);
         it != room.kept.end(); ++it) {
        if (c.client->unsent_bytes() > limits_.lossy_backlog) {
            s->behind = next;
            c.behind.push_back(room.id);
            return;
        }
        // Seqs from the cursor that the room does not keep: a message that could not be kept,
        // or a gap of the room itself. Either way the client is moved past them.
        counters_.lossy_drops += it->seq - next;
        next = it->seq;
        out.clear();
        write_message(out, rt::Message{.room = room.id,
                                       .seq = it->seq,
                                       .sender = it->sender,
                                       .key = it->key,
                                       .body = it->body});
        if (!c.client->push(out)) {
            // Closing: what is left is counted when it detaches.
            s->behind = next;
            return;
        }
        ++counters_.delivered;
        next = it->seq + 1;
    }
    counters_.lossy_drops += owed(next, room.head);
    s->behind.reset();
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
    ++kept_messages_;
    while (room.kept_bytes > limits_.room_buffer_bytes) {
        drop_oldest(room);
    }
    while (!kept_order_.empty() &&
           (buffered_bytes_ > limits_.buffer_bytes || kept_messages_ > limits_.buffer_messages)) {
        forget_oldest();
    }
    // A room dropping its own oldest (a busy live chat does, every message) leaves its entry
    // here behind. Past a quarter over the bound at least that quarter is such, so clearing them
    // out costs five entries looked at per entry cleared, nothing that grows per message, and
    // the entries never count against rooms that keep theirs.
    if (kept_order_.size() > limits_.buffer_messages + (limits_.buffer_messages / 4)) {
        std::erase_if(kept_order_, [this](const std::pair<core::RoomId, std::uint64_t>& e) {
            const Room* r = find(e.first);
            return r == nullptr || r->kept.empty() || e.second < r->kept.front().seq;
        });
    }
}

void ChatService::drop_oldest(Room& room) noexcept {
    const std::uint64_t seq = room.kept.front().seq;
    for (Room::Subscriber& s : room.subscribers) {
        if (s.behind && *s.behind <= seq) {
            counters_.lossy_drops += seq + 1 - *s.behind;
            s.behind = seq + 1;
        }
    }
    const std::size_t cost = room.kept.front().body.size() + kMessageOverhead;
    room.kept_bytes -= cost;
    buffered_bytes_ -= cost;
    --kept_messages_;
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
    ask_rechecks();
    const core::MonoTime now = clock_.now();
    if (now < next_sweep_) {
        return;
    }
    next_sweep_ = now + kSweepEvery;
    // Checks a full queue or a failed allocation left out: a resync of its own, once the one
    // before is done.
    if (resync_owed_ && rechecks_.empty() && rechecks_in_flight_ == 0) {
        on_members_resync();
    }
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

void ChatService::leave(ClientId id, Client& c, const core::RoomId& room) noexcept {
    std::erase(c.rooms, room);
    std::erase(c.behind, room);
    Room* r = find(room);
    if (r == nullptr) {
        return;
    }
    if (const auto s = std::ranges::find(r->subscribers, id, &Room::Subscriber::id);
        s != r->subscribers.end()) {
        counters_.lossy_drops += owed(s->behind, r->head);
        r->subscribers.erase(s);
    }
    std::erase_if(r->waiting, [id](const Room::Waiting& w) { return w.id == id; });
    if (r->subscribers.empty() && r->waiting.empty()) {
        r->unused_since = clock_.now();
    }
}

void ChatService::on_member_removed(const core::RoomId& room, const core::UserId& user) noexcept {
    if (core::ports::is_stream_chat(room)) {
        return;
    }
    for (auto& [value, c] : clients_) {
        if (c.user != user || std::ranges::find(c.rooms, room) == c.rooms.end()) {
            continue;
        }
        if (std::ranges::find(c.admitting, room) != c.admitting.end()) {
            if (std::ranges::find(c.revoked, room) == c.revoked.end()) {
                try {
                    c.revoked.push_back(room);
                } catch (const std::bad_alloc&) {
                    // Without the mark the answer would admit it: the client goes instead.
                    ++counters_.allocation_failures;
                    c.client->allocation_failed();
                }
            }
            continue;
        }
        leave(ClientId{value}, c, room);
        ++counters_.removals;
        answer(*c.client, "not_member", room);
    }
}

void ChatService::on_members_resync() noexcept {
    // What an earlier resync still owes is covered by this one, asked after the store listens
    // again: its checks may have been read before a removal this one is for.
    ++recheck_generation_;
    rechecks_.clear();
    rechecks_queued_.clear();
    resync_owed_ = false;
    try {
        for (auto& [value, c] : clients_) {
            for (const core::RoomId& room : c.rooms) {
                if (core::ports::is_stream_chat(room)) {
                    continue;
                }
                // Its list is being read now, perhaps from before the removal: asked again once
                // the answer lets it in, when the room is recorded.
                if (std::ranges::find(c.admitting, room) != c.admitting.end()) {
                    if (std::ranges::find(c.unchecked, room) == c.unchecked.end()) {
                        c.unchecked.push_back(room);
                    }
                    continue;
                }
                if (!recheck(room, c.user)) {
                    resync_owed_ = true;
                }
            }
        }
    } catch (const std::bad_alloc&) {
        // Those not queued now are asked by another resync once these are done.
        ++counters_.allocation_failures;
        resync_owed_ = true;
    }
    ask_rechecks();
}

std::size_t ChatService::RecheckHash::operator()(const Recheck& r) const noexcept {
    const std::size_t room = std::hash<core::RoomId>{}(r.first);
    return room ^ (std::hash<core::UserId>{}(r.second) + 0x9e3779b97f4a7c15U + (room << 6U) +
                   (room >> 2U));
}

bool ChatService::recheck(const core::RoomId& room, const core::UserId& user) {
    if (rechecks_.size() >= kMaxRechecks) {
        return false;
    }
    Recheck pair{room, user};
    if (rechecks_queued_.contains(pair)) {
        return true;
    }
    rechecks_.push_back(pair);
    try {
        rechecks_queued_.insert(std::move(pair));
    } catch (const std::bad_alloc&) {
        rechecks_.pop_back();
        throw;
    }
    return true;
}

void ChatService::ask_rechecks() noexcept {
    // A check answered from inside admits comes back here: the loop below asks the next.
    if (asking_rechecks_) {
        return;
    }
    asking_rechecks_ = true;
    while (!rechecks_.empty() && rechecks_in_flight_ < kRechecksInFlight &&
           clock_.now() >= rechecks_resume_) {
        Recheck pair = std::move(rechecks_.front());
        rechecks_.pop_front();
        rechecks_queued_.erase(pair);
        ++rechecks_in_flight_;
        try {
            // A recorded room, so the check records nothing: its kind was recorded by the join
            // that let the client in.
            messages_.admits(pair.first, pair.second, core::ports::RoomKind::GroupChat,
                             [this, pair, generation = recheck_generation_](
                                 core::ports::MessageResult<core::ports::Admission> r) noexcept {
                                 rechecked(pair, generation, r);
                             });
        } catch (const std::bad_alloc&) {
            // Not asked: the next sweep asks again.
            ++counters_.allocation_failures;
            --rechecks_in_flight_;
            rechecks_resume_ = clock_.now() + kSweepEvery;
            try {
                if (!recheck(pair.first, pair.second)) {
                    resync_owed_ = true;
                }
            } catch (const std::bad_alloc&) {
                resync_owed_ = true;
            }
        }
    }
    asking_rechecks_ = false;
}

void ChatService::rechecked(const Recheck& pair, std::uint64_t generation,
                            core::ports::MessageResult<core::ports::Admission> result) noexcept {
    --rechecks_in_flight_;
    if (!result) {
        // The store is still down, or went down again: the check waits a second, and so do the
        // rest. A later resync asks it anyway.
        rechecks_resume_ = clock_.now() + kSweepEvery;
        if (generation == recheck_generation_) {
            try {
                if (!recheck(pair.first, pair.second)) {
                    resync_owed_ = true;
                }
            } catch (const std::bad_alloc&) {
                ++counters_.allocation_failures;
                resync_owed_ = true;
            }
        }
        return;
    }
    if (*result == core::ports::Admission::NotMember) {
        on_member_removed(pair.first, pair.second);
    }
    ask_rechecks();
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
    kept_messages_ -= r.kept.size();
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
