// Read receipts and typing (ADR-0138): a member's read mark, stored and told to the room's joined
// sockets; and that a member is typing, told to them and never stored. Both go to the room itself
// as notices (rt::RoomRouter::notify), which reach every node with sockets in it; on_notice
// (chat_service.cpp) pushes them there.
#include "chat_service.hpp"

#include <algorithm>
#include <new>
#include <utility>

namespace chat {

ChatService::Client* ChatService::in_room(ClientId id, const core::RoomId& room) noexcept {
    Client* c = find(id);
    if (c == nullptr) {
        return nullptr;
    }
    if (std::ranges::find(c->rooms, room) == c->rooms.end() ||
        std::ranges::find(c->admitting, room) != c->admitting.end()) {
        answer(*c->client, reason(rt::RouteError::NotJoined), room);
        return nullptr;
    }
    // A stream's live chat has no member list to mark, and an audience too large to tell who is
    // typing.
    if (core::ports::is_stream_chat(room)) {
        answer(*c->client, "not_allowed", room);
        return nullptr;
    }
    return c;
}

void ChatService::typing(ClientId id, const Typing& typing) {
    Client* c = in_room(id, typing.room);
    if (c == nullptr) {
        return;
    }
    const core::MonoTime now = clock_.now();
    if (!c->typing) {
        c->typing.emplace(limits_.typing_burst, limits_.typing_interval, now);
    }
    // Lossy: one too many is dropped, unanswered; the next keystroke a second on says it again.
    if (!c->typing->available(now)) {
        ++counters_.typing_dropped;
        return;
    }
    c->typing->take(now);
    rooms_plane_.notify(typing.room, encode_typing({.room = typing.room, .user = c->user}));
    ++counters_.typing_sent;
}

void ChatService::mark_read(ClientId id, const MarkRead& read) {
    Client* c = in_room(id, read.room);
    if (c == nullptr) {
        return;
    }
    // What this connection's marks already reached is not written again: a client that says
    // read for every message it shows writes once for each that moves its mark.
    const auto mark =
        std::ranges::find(c->marked, read.room, &std::pair<core::RoomId, std::uint64_t>::first);
    if (mark != c->marked.end() && mark->second >= read.seq) {
        return;
    }
    const core::MonoTime now = clock_.now();
    const auto bucket =
        reads_.try_emplace(c->user, limits_.read_burst, limits_.reads_per_second, now).first;
    if (const auto taken = bucket->second.take(now); !taken) {
        ++counters_.reads_rate_limited;
        refuse(*c->client, "rate_limited",
               {.room = read.room,
                .id = std::nullopt,
                .user = std::nullopt,
                .retry_after = taken.error()});
        return;
    }
    if (stopped_) {
        return;
    }
    messages_.mark_read(
        read.room, c->user, read.seq,
        [this, id, room = read.room,
         user = c->user](core::ports::MessageResult<std::optional<std::uint64_t>> result) noexcept {
            read_marked(id, room, user, result);
        });
}

void ChatService::read_marked(
    ClientId id, const core::RoomId& room, const core::UserId& user,
    const core::ports::MessageResult<std::optional<std::uint64_t>>& result) noexcept {
    Client* c = find(id);
    if (!result) {
        ++counters_.reads_failed;
        if (c != nullptr) {
            answer(*c->client, "unavailable", room);
        }
        return;
    }
    // Not listed, or the mark is there already: nothing changed, and nobody is told.
    const std::optional<std::uint64_t> moved = *result;
    if (!moved) {
        ++counters_.reads_unmoved;
        return;
    }
    ++counters_.reads_marked;
    try {
        if (c != nullptr && std::ranges::find(c->rooms, room) != c->rooms.end()) {
            const auto mark =
                std::ranges::find(c->marked, room, &std::pair<core::RoomId, std::uint64_t>::first);
            if (mark == c->marked.end()) {
                c->marked.emplace_back(room, *moved);
            } else {
                mark->second = std::max(mark->second, *moved);
            }
        }
        rooms_plane_.notify(room, encode_read({.room = room, .user = user, .seq = *moved}));
    } catch (const std::bad_alloc&) {
        // The mark is stored; the room hears of it when reads is next asked.
        ++counters_.allocation_failures;
    }
}

void ChatService::list_reads(ClientId id, const ListReads& reads) {
    Client* c = in_room(id, reads.room);
    if (c == nullptr) {
        return;
    }
    // A read of the store, as a history page is.
    if (!admit_join(c->user)) {
        answer(*c->client, "busy", reads.room);
        return;
    }
    if (stopped_) {
        return;
    }
    messages_.read_marks(
        reads.room,
        [this, id, room = reads.room](
            core::ports::MessageResult<std::vector<core::ports::ReadMark>> marks) noexcept {
            Client* asker = find(id);
            if (asker == nullptr) {
                return;
            }
            if (!marks) {
                answer(*asker->client, "unavailable", room);
                return;
            }
            try {
                std::string out;
                write_reads(out, room, *marks);
                asker->client->push(out);
                ++counters_.reads_listed;
            } catch (const std::bad_alloc&) {
                ++counters_.allocation_failures;
                asker->client->allocation_failed();
            }
        });
}

} // namespace chat
