#pragma once

#include "core/models/ids.hpp"
#include "core/ports/clock.hpp"
#include "core/ports/message_store.hpp"
#include "rt/message_key.hpp"
#include "rt/room_router.hpp"

#include "envelope.hpp"
#include "token_bucket.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chat {

// A client connection as the chat service sees it. Called on the reactor thread.
class IClient {
public:
    virtual ~IClient() = default;
    // One envelope message; false when the connection is closing and it went nowhere. Never
    // closes the connection from inside the call, which may be running inside the router's
    // fan-out; a connection that cannot take it closes later.
    virtual bool push(std::string_view text) noexcept = 0;
    // What the connection has queued that its peer has not read yet.
    [[nodiscard]] virtual std::size_t unsent_bytes() const noexcept = 0;
    // Something this client asked for could not be allocated; it pays with its connection
    // (ADR-0036), later, as push() does.
    virtual void allocation_failed() noexcept = 0;
};

// The room plane: rt::RoomRouter, or a fake in tests.
class IRooms {
public:
    virtual ~IRooms() = default;
    virtual void join(const core::RoomId& room, rt::IMember& member, rt::JoinCallback done) = 0;
    virtual void leave(const core::RoomId& room, rt::IMember& member) noexcept = 0;
    virtual void send(const core::RoomId& room, rt::IMember& from, const core::UserId& sender,
                      const rt::MessageKey& key, std::vector<std::byte> body,
                      rt::SendCallback done) = 0;
};

struct ServiceLimits {
    // A client shows a handful of conversations at once; 64 bounds what one socket makes this
    // node track and subscribe to.
    std::size_t max_rooms_per_client = 64;
    // Joins of new rooms per user, across all their connections: a join may create the room,
    // a row that outlives everyone in it. A fresh user may fill one connection's rooms at once;
    // after that one a second, far faster than a person opens conversations and far slower
    // than a script filling the table would like.
    std::uint32_t join_burst = 64;
    std::uint32_t joins_per_second = 1;
    // Messages per user, across all their connections on this node. Someone typing sends a
    // message every few seconds; a burst is a pasted paragraph split in lines or a quick run of
    // replies, rarely past five. 10 covers that twice over. Sustained, 2 a second is 120 a
    // minute, several times the fastest typist, while a script held to it is a trickle next to
    // the thousand a second a room can sequence (ADR-0035).
    std::uint32_t send_burst = 10;
    std::uint32_t sends_per_second = 2;
    // Bytes of a client's sends not yet answered, each counted as its body plus 256 for the
    // rest of it. They sit in an owner's queue or on the node channel meanwhile, so they are
    // part of the connection's memory: two of the largest messages, or dozens of ordinary ones.
    std::size_t max_send_bytes_in_flight = std::size_t{128} * 1024;
    // A lossy client this far behind is given nothing more until its connection has drained;
    // then it is sent the messages it missed that are still owed, oldest first. A quarter of
    // the backlog that closes a connection (Limits::max_backlog), so one largest delivery on
    // top still leaves a lossy client open.
    std::size_t lossy_backlog = std::size_t{64} * 1024;
    // What a lossy client that fell behind is owed at most: the newest this many of the room's
    // messages, and only those the room still keeps. Older ones are dropped, oldest first, and
    // counted. A phone shows 15 to 20 lines of chat; three screens of the newest is what a
    // viewer who stalled wants to see again, and anything older is history (ADR-0057).
    std::uint64_t lossy_depth = 64;
    // What a client's resumes may queue on its connection within one linger: half the backlog
    // that closes it, so that resuming cannot itself get the client closed. Messages past it
    // are left out, oldest first; the client sees the gap.
    std::size_t replay_budget = std::size_t{128} * 1024;
    // Each room this node is in keeps its latest messages for clients that come back, each
    // counted as its body plus 256. 256 KiB is a few hundred ordinary messages: what a phone
    // that dropped off for a minute missed in a busy conversation. All rooms together keep at
    // most 32 MiB, and 131072 messages (the order they are dropped in costs 24 bytes each, with
    // room for as many again of messages their own room dropped first: 6 MiB at most).
    std::size_t room_buffer_bytes = std::size_t{256} * 1024;
    std::size_t buffer_bytes = std::size_t{32} << 20U;
    std::size_t buffer_messages = std::size_t{128} * 1024;
    // A stream's live chat (ADR-0057). Its owner sequences about a thousand messages a second
    // (ADR-0035) for every node's senders, and an audience of thousands, each within their own
    // two a second, would ask for far more and have the owner turn everyone away as busy. So
    // each node lets 20 a second into the room, 40 at once: three nodes make 60 a second, more
    // than anyone reads and 18 KB/s to each viewer at 300 bytes a message, and the owner's
    // ceiling holds up to sixteen nodes with room to spare.
    std::uint32_t live_room_burst = 40;
    std::uint32_t live_room_sends_per_second = 20;
    // A live chat message is a line: 500 characters, what the large platforms allow, of up to
    // four UTF-8 bytes each. Bounded so that a room's kept messages hold the lossy_depth a
    // viewer is owed (64 x (2000 + 256) is 141 KiB of the 256) and a live room's stored
    // messages stay small.
    std::size_t live_body = 2'000;
    // How long a room stays joined, and its messages kept, after its last client here left:
    // a page reload, a network switch, or a slow reader's reconnect take seconds; a client's
    // backoff reaches half a minute after a few failures.
    core::Millis linger{30'000};
};

struct ServiceCounters {
    std::uint64_t delivered = 0;
    std::uint64_t rate_limited = 0;
    // Messages a lossy client was moved past, never to be sent them, because it was behind.
    std::uint64_t lossy_drops = 0;
    // Messages sent again to a client resuming a room.
    std::uint64_t replayed = 0;
    // Messages sent to clients from history.
    std::uint64_t history_messages = 0;
    std::uint64_t allocation_failures = 0;
};

// Identifies an attached client; never reused while the service lives.
struct ClientId {
    std::uint64_t value = 0;
    friend bool operator==(ClientId, ClientId) = default;
};

// Policy between the client edge and the room plane: who may send how much, which rooms each
// client hears, how a delivery reaches a client that is behind, and what a client that comes
// back is sent again. It joins each room once for all its clients on this node, keeps the
// room's latest messages, and hands each message to every client in the room. Bodies are
// opaque bytes here: carried, kept and passed on, never read. Everything runs on the reactor
// thread.
class ChatService {
public:
    // `messages` answers whether a room admits a user. Its answers must never reach a destroyed
    // service: destroy the store first, which drops what it still owes.
    ChatService(IRooms& rooms, core::ports::IMessageStore& messages,
                const core::ports::IClock& clock, ServiceLimits limits);
    // Leaves every room, which also drops what the room plane owes for sends still in flight:
    // nothing calls back into a destroyed service. The room plane must outlive it.
    ~ChatService();
    ChatService(const ChatService&) = delete;
    ChatService& operator=(const ChatService&) = delete;
    ChatService(ChatService&&) = delete;
    ChatService& operator=(ChatService&&) = delete;

    [[nodiscard]] ClientId attach(IClient& client, const core::UserId& user);
    // Takes the client out of its rooms; nothing reaches it after this.
    void detach(ClientId id) noexcept;
    void join(ClientId id, const Join& join);
    void send(ClientId id, Send send);
    // A page of the room's stored messages, as message frames and then a history frame, for a
    // client in the room. Cut short to what the client can take while it is behind.
    void history(ClientId id, const History& history);
    // The client's connection has sent everything it had queued: a lossy client that fell
    // behind is sent what it is still owed.
    void drained(ClientId id) noexcept;
    // Leaves the rooms no client here has used for `linger`. Cheap to call often.
    void sweep() noexcept;

    [[nodiscard]] const ServiceCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] std::size_t rooms() const noexcept { return rooms_.size(); }
    [[nodiscard]] std::size_t buffered_bytes() const noexcept { return buffered_bytes_; }

private:
    struct Room;

    struct Client {
        IClient* client;
        core::UserId user;
        std::vector<core::RoomId> rooms;
        // Of `rooms`, those whose member list has not answered yet.
        std::vector<core::RoomId> admitting;
        // Of `rooms`, those in which it is lossy and behind.
        std::vector<core::RoomId> behind;
        std::size_t send_bytes_in_flight = 0;
        // What resumes queued since the window started.
        std::size_t replayed_bytes = 0;
        core::MonoTime replay_window_start;
    };

    [[nodiscard]] Client* find(ClientId id) noexcept;
    [[nodiscard]] Room* find(const core::RoomId& room) noexcept;
    [[nodiscard]] bool admit_join(const core::UserId& user);
    void admitted(ClientId id, const Join& join,
                  core::ports::MessageResult<core::ports::Admission> result) noexcept;
    void enter(ClientId id, const Join& join);
    void joined(const core::RoomId& room,
                std::expected<std::uint64_t, rt::RouteError> result) noexcept;
    void delivered(Room& room, const rt::Message& message) noexcept;
    void sent(ClientId id, const core::RoomId& room, const rt::MessageKey& key, std::size_t bytes,
              std::expected<std::uint64_t, rt::RouteError> result) noexcept;
    void
    page_read(ClientId id, const core::RoomId& room,
              core::ports::MessageResult<std::vector<core::ports::StoredMessage>> page) noexcept;
    void subscribe(Room& room, ClientId id, const Join& join);
    void fell_behind(ClientId id, const core::RoomId& room) noexcept;
    void catch_up(Room& room, ClientId id, Client& c);
    void replay(const Room& room, Client& c, std::uint64_t after);
    void keep(Room& room, const rt::Message& message);
    void drop_oldest(Room& room) noexcept;
    void forget_oldest() noexcept;
    void erase(const core::RoomId& room) noexcept;
    void answer(IClient& client, std::string_view reason, const core::RoomId& room,
                const std::optional<rt::MessageKey>& id = std::nullopt) noexcept;

    IRooms& rooms_plane_;
    core::ports::IMessageStore& messages_;
    const core::ports::IClock& clock_;
    ServiceLimits limits_;
    ServiceCounters counters_;
    std::uint64_t next_client_ = 1;
    std::unordered_map<std::uint64_t, Client> clients_;
    std::unordered_map<core::RoomId, std::unique_ptr<Room>> rooms_;
    std::unordered_map<core::UserId, TokenBucket> joins_;
    std::unordered_map<core::UserId, TokenBucket> sends_;
    // Every kept message, in the order kept, to drop the oldest of all rooms first.
    std::deque<std::pair<core::RoomId, std::uint64_t>> kept_order_;
    std::size_t buffered_bytes_ = 0;
    // Messages kept across rooms; kept_order_ may also hold entries of messages already gone.
    std::size_t kept_messages_ = 0;
    core::MonoTime next_sweep_;
};

} // namespace chat
