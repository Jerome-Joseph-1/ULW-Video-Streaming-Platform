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

// Section 8.15's room kinds, as far as who may be in the room goes.
enum class RoomKind : std::uint8_t {
    DirectChat,
    GroupChat,
    StreamLiveChat,
};

// Whether a room of `kind` admits a user who is not among its members.
[[nodiscard]] constexpr bool admits_anyone(RoomKind kind) noexcept {
    switch (kind) {
    case RoomKind::DirectChat:
    case RoomKind::GroupChat:
        return false;
    case RoomKind::StreamLiveChat:
        return true;
    }
    return false;
}

// Rooms named by something else, their id derived from its name (ADR-0056, ADR-0070): an RFC
// 9562 version 8 UUID whose first byte says what names it, the rest a digest of the name. Every
// other room id is version 7 (ADR-0023), so no id is taken for another's.
enum class NamedRoom : std::uint8_t {
    // A live stream's chat, from the stream's name (apps/chat/src/live_chat.cpp).
    StreamChat = 0x01,
    // A user's presence room, from the user's id (apps/chat/src/presence_room.cpp).
    Presence = 0x02,
};

// Whether the room's id was derived from a name of `kind`.
[[nodiscard]] inline bool is_named_room(const RoomId& room, NamedRoom kind) noexcept {
    // RFC 9562 section 4: the version is the high nibble of byte 6.
    constexpr std::size_t kVersionByte = 6;
    constexpr unsigned kVersion8 = 0x80U;
    const auto bytes = room.uuid().bytes();
    return (std::to_integer<unsigned>(bytes[kVersionByte]) & 0xF0U) == kVersion8 &&
           std::to_integer<std::uint8_t>(bytes[0]) == static_cast<std::uint8_t>(kind);
}

// Whether the room is a stream's live chat, by its id alone: only such a room is ever recorded
// as StreamLiveChat, so the id says which rooms get the live chat's bounds, on every node, with
// no lookup.
[[nodiscard]] inline bool is_stream_chat(const RoomId& room) noexcept {
    return is_named_room(room, NamedRoom::StreamChat);
}

// What admits answers for a join.
enum class Admission : std::uint8_t {
    Admitted,
    // The room is closed and the user is not among its members.
    NotMember,
    // The join asked for the open kind, and the room is not recorded as open.
    NotLive,
};

// What a join that asked for `asked` is answered in a room recorded as `recorded`, of which the
// user is a member or not. Asking for the open kind never opens a closed room.
[[nodiscard]] constexpr Admission admission(RoomKind asked, RoomKind recorded,
                                            bool member) noexcept {
    if (admits_anyone(recorded)) {
        return Admission::Admitted;
    }
    if (admits_anyone(asked)) {
        return Admission::NotLive;
    }
    return member ? Admission::Admitted : Admission::NotMember;
}

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
// (ADR-0054), so no seq is ever taken without its message; this port has no writer of its own.
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

    // Both idempotent. Adding a member to a room with no kind recorded records it as a group
    // chat, so that the room is closed before it lists anyone and record_live, which waits for
    // that record, cannot open it.
    virtual void add_member(const RoomId& room, const UserId& user, MessageCallback<void> done) = 0;
    virtual void remove_member(const RoomId& room, const UserId& user,
                               MessageCallback<void> done) = 0;
    // Members above `after` in byte order of their ids, at most min(limit, kMaxMembersPage).
    virtual void members(const RoomId& room, std::optional<UserId> after, std::size_t limit,
                         MessageCallback<std::vector<UserId>> done) = 0;
    // Whether `user` may be in the room, by the room's kind: a direct or group chat admits only
    // its members, even while it has none; a stream's live chat admits anyone. A join never widens
    // who may be in a room: it records only a closed kind, on a room that has none recorded, and
    // otherwise takes the recorded kind. It answers NotLive, and records nothing, when it asks for
    // the open kind of a room that is not recorded as open; only record_live opens a room.
    virtual void admits(const RoomId& room, const UserId& user, RoomKind asked,
                        MessageCallback<Admission> done) = 0;
    // Records the room as a stream's live chat, which admits anyone: a server-side step (the
    // stream's owner opening its chat), never a client's. Conflict, and nothing recorded, when the
    // room is not a stream's chat (is_stream_chat), lists members, is recorded as another kind,
    // or was created on the room plane as another kind (whose kind and delivery are fixed when it
    // is created); recording it again does nothing. The in-memory store keeps no room plane, so
    // only the first three apply to it.
    virtual void record_live(const RoomId& room, MessageCallback<void> done) = 0;
};

} // namespace core::ports
