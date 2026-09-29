#pragma once

#include "core/models/ids.hpp"
#include "core/util/time.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace core::ports {

// A client message is at most 64 KiB (the WebSocket decoder's bound, ADR-0029), and the body is
// part of it, so no body a client can send is larger. MLS ciphertext and commits (ADR-0016) are
// bodies like any other and fit the same bound.
inline constexpr std::size_t kMaxMessageBody = std::size_t{64} * 1024;

// A page of history goes to one client, whose unsent output is capped at 256 KiB before it is
// closed as a slow reader (ADR-0036). A page therefore stops once its bodies reach that much,
// and at 256 rows either way: a line of chat is well under a kilobyte, so 256 of them are
// about one screenful of scrollback and still inside the byte bound.
inline constexpr std::size_t kMaxHistoryBytes = std::size_t{256} * 1024;
inline constexpr std::size_t kMaxHistoryRows = 256;
// The id a sender gives each message, so that sending it again is recognised as the same
// message: at most 64 characters, as the room plane's message key allows.
inline constexpr std::size_t kMaxMessageKey = 64;
// A user id is at most 128 bytes: 1024 of them are 128 KiB, the size of a history page's
// share of that same client budget.
inline constexpr std::size_t kMaxMembersPage = 1024;

enum class MessageStoreError : std::uint8_t {
    // Unreachable, timed out, or lost a race with a concurrent write; the call may be repeated.
    Unavailable,
    // A write that disagrees with what is stored under the same seq. Nothing was written.
    Conflict,
    // The body is larger than kMaxMessageBody. Nothing was written.
    TooLarge,
    // A stored row that no writer here produces.
    Corrupt,
};

template <class T> using MessageResult = std::expected<T, MessageStoreError>;
// Called exactly once, on the reactor thread, never from inside the call that was given it.
template <class T> using MessageCallback = std::move_only_function<void(MessageResult<T>) noexcept>;

struct StoredMessage {
    std::uint64_t seq = 0;
    UserId sender;
    // The sender's id for the message, unique per room and sender.
    std::string key;
    // Microsecond precision: what the store keeps.
    WallTime sent_at;
    std::vector<std::byte> body;

    friend bool operator==(const StoredMessage&, const StoredMessage&) = default;
};

// Where a room's sequenced messages are read back, and its members kept. Bodies are opaque
// bytes, end to end: they are stored and returned exactly, and never parsed, logged or indexed.
// Plaintext today and MLS ciphertext later are the same thing to the store.
//
// Messages are written by the room's owner, in the same fenced write that takes their seq
// (ADR-0046), so no seq is ever taken without its message; this port has no writer of its own.
class IMessageStore {
public:
    virtual ~IMessageStore() = default;

    // Messages below `before` (the newest when nullopt), newest first: scrolling back.
    // Messages above `after`, oldest first: resuming from the last seq a client saw.
    // Each page holds at most min(limit, kMaxHistoryRows) messages whose bodies add up to at
    // most kMaxHistoryBytes, and at least one message whenever one is left, however large.
    // Pages are contiguous: a gap in seq is a seq never stored.
    virtual void history_before(const RoomId& room, std::optional<std::uint64_t> before,
                                std::size_t limit,
                                MessageCallback<std::vector<StoredMessage>> done) = 0;
    virtual void history_after(const RoomId& room, std::uint64_t after, std::size_t limit,
                               MessageCallback<std::vector<StoredMessage>> done) = 0;

    // The room's sequence counter, the one source of the last seq its owner took; 0 for a room
    // with none. A seq taken with its message is also the newest stored one.
    virtual void last_seq(const RoomId& room, MessageCallback<std::uint64_t> done) = 0;

    // Both idempotent.
    virtual void add_member(const RoomId& room, const UserId& user, MessageCallback<void> done) = 0;
    virtual void remove_member(const RoomId& room, const UserId& user,
                               MessageCallback<void> done) = 0;
    // Members above `after` in byte order of their ids, at most min(limit, kMaxMembersPage).
    virtual void members(const RoomId& room, std::optional<UserId> after, std::size_t limit,
                         MessageCallback<std::vector<UserId>> done) = 0;
    // Whether `user` may be in the room: a room with members admits only them; a room with none
    // is open to anyone, as a stream's live chat is.
    virtual void admits(const RoomId& room, const UserId& user, MessageCallback<bool> done) = 0;
};

} // namespace core::ports
