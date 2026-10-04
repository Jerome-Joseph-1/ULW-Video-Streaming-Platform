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
    // A direct chat, from the pair of its two users (apps/chat/src/named_rooms.cpp, ADR-0096):
    // the same pair always names the same room.
    DirectChat = 0x03,
    // A group chat, from its creator and the id the creator gave the request (ADR-0096): a
    // repeated create names the room the first one made.
    GroupChat = 0x04,
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

// The kind a room named by a pair or a creator is, by its id alone; nullopt for any other id. A
// join of such a room asks for this kind whatever it says, and a room with no kind recorded is
// created as it, so nothing can record it as another (ADR-0096).
[[nodiscard]] inline std::optional<RoomKind> named_kind(const RoomId& room) noexcept {
    if (is_named_room(room, NamedRoom::DirectChat)) {
        return RoomKind::DirectChat;
    }
    if (is_named_room(room, NamedRoom::GroupChat)) {
        return RoomKind::GroupChat;
    }
    return std::nullopt;
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

// What a room is recorded as, and whether a user is on its member list: what a call's handler
// checks on the room's owner before it hands out a ticket (ADR-0050). Unlike admits, it records
// nothing and widens nothing: a room with no kind recorded has no kind here.
struct RoomAccess {
    std::optional<RoomKind> kind;
    bool member = false;

    friend bool operator==(const RoomAccess&, const RoomAccess&) = default;
};

// What a member may do to a room's member list (ADR-0096). A group chat's creator is its first
// admin; admins add and remove members; anyone listed may leave a group chat. Members of a direct
// chat, and rows an operator inserts without a role, are plain members.
enum class MemberRole : std::uint8_t { Member, Admin };

// A small group (ADR-0016): each member's device is an MLS member, and every commit reaches all
// of them. Counted with the creator and every admin.
inline constexpr std::size_t kMaxGroupMembers = 100;
// Users one create or add names at once, besides the one asking: the answer lists those it
// added, and each is a notification on every chat node.
inline constexpr std::size_t kMaxMembersPerChange = 50;
// A page of a user's rooms, or of a room's members with their roles: each entry is well under
// 300 bytes on the client's socket, so a page stays inside a history page's share of its budget.
// The stores answer one entry more than this at most, so that a caller asking for a full page
// and one more can tell whether another page follows.
inline constexpr std::size_t kMaxListPage = 100;
// The rooms a user may be listed in and still open a direct chat or create a group: what one
// account can make the database hold, whichever chat node it asks and however long it takes.
// Being added by others is not bounded by it, so nobody can lock someone else out.
inline constexpr std::size_t kMaxRoomsPerUser = 1000;

// One of a user's rooms, as rooms_of lists them.
struct RoomEntry {
    RoomId room;
    // A room listed with no kind recorded (ADR-0075's race) is a group chat, as a join records it.
    RoomKind kind = RoomKind::GroupChat;
    MemberRole role = MemberRole::Member;
    // The other member of a direct chat, while one is listed.
    std::optional<UserId> peer;

    friend bool operator==(const RoomEntry&, const RoomEntry&) = default;
};

struct MemberEntry {
    UserId user;
    MemberRole role = MemberRole::Member;

    friend bool operator==(const MemberEntry&, const MemberEntry&) = default;
};

// A page of a room's members, read for one of them. `asker_listed` false: the asker is not on the
// list, and `members` is empty, whatever the list holds.
struct Roster {
    bool asker_listed = false;
    std::vector<MemberEntry> members;

    friend bool operator==(const Roster&, const Roster&) = default;
};

// What a member-list change asked by a user answers when the store could be asked. Every
// refusal wrote nothing.
enum class MembershipOutcome : std::uint8_t {
    Done,
    // The user asking is not on the room's list (nor is the room recorded at all).
    NotMember,
    // Only an admin may add or remove others.
    NotAdmin,
    // The room is not a group chat: a direct chat's two never change, and a stream's live chat
    // has no list to manage.
    NotGroup,
    // The change would take the group past kMaxGroupMembers.
    Full,
    // open_direct or create_group of a room recorded as another kind.
    WrongKind,
    // open_direct or create_group that would list a room for a user listed in
    // kMaxRoomsPerUser rooms already.
    RoomLimit,
    // create_group of a room that lists nobody any more but holds messages: its members all
    // left, and listing new ones would hand them the history. The request needs a new id.
    Gone,
};

struct MembershipChange {
    MembershipOutcome outcome = MembershipOutcome::Done;
    // Done: who the change listed or took off, in byte order of their ids; empty when it changed
    // nothing (a repeat).
    std::vector<UserId> changed;
    // Done, by a leave or a removal: who became the room's admin because the last one went.
    std::optional<UserId> promoted;

    friend bool operator==(const MembershipChange&, const MembershipChange&) = default;
};

// Who changes or reads a member list (ADR-0096): a user, held to their own place on the list, or
// the operator's backend through chat's service API, which adds to and reads any group's list as
// its admin would without being on it. A user converts implicitly, so that every call a user makes
// reads as before.
class Actor {
public:
    Actor(const UserId& user) : user_(user) {}

    [[nodiscard]] static Actor service() noexcept { return Actor{}; }

    // The user acting; nullopt for the service.
    [[nodiscard]] const std::optional<UserId>& user() const noexcept { return user_; }
    [[nodiscard]] bool is_service() const noexcept { return !user_.has_value(); }

private:
    Actor() noexcept = default;

    std::optional<UserId> user_;
};

// The users one shared_with call asks about at most: a user's watches on one node (16 sockets of
// 128 watches, apps/chat/src/presence.hpp).
inline constexpr std::size_t kMaxSharedAsked = 2048;

// Whether a join of a room with no kind recorded may record one. Skipped answers exactly as
// Allowed would, and writes nothing: how the chat service bounds the rows a user's joins create.
enum class Recording : std::uint8_t { Allowed, Skipped };

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

// Told when a member list shrinks, on the reactor thread, never from inside a store call.
class IMemberListener {
public:
    virtual ~IMemberListener() = default;
    // `user` is no longer on the member list of `room`, whoever changed it and however: an
    // operator's DELETE in the database counts as much as remove_member.
    virtual void on_member_removed(const RoomId& room, const UserId& user) noexcept = 0;
    // `user` is on the member list of `room` now, whoever listed them and however.
    virtual void on_member_added(const RoomId& room, const UserId& user) noexcept = 0;
    // `user`, on the member list of `room`, has `role` now.
    virtual void on_member_role(const RoomId& room, const UserId& user,
                                MemberRole role) noexcept = 0;
    // Changes may have gone unannounced (the store lost its way to hear them): whatever relies
    // on them must check the member lists it cares about again.
    virtual void on_members_resync() noexcept = 0;
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
    // the open kind of a room that is not recorded as open; only record_live opens a room. With
    // Recording::Skipped, a room with no kind recorded is answered as if `asked` were recorded,
    // and stays unrecorded.
    virtual void admits(const RoomId& room, const UserId& user, RoomKind asked, Recording recording,
                        MessageCallback<Admission> done) = 0;
    void admits(const RoomId& room, const UserId& user, RoomKind asked,
                MessageCallback<Admission> done) {
        admits(room, user, asked, Recording::Allowed, std::move(done));
    }
    // The room's recorded kind and whether `user` is listed in it, read only.
    virtual void access(const RoomId& room, const UserId& user,
                        MessageCallback<RoomAccess> done) = 0;
    // Records the room as a stream's live chat, which admits anyone: a server-side step (the
    // stream's owner opening its chat), never a client's. Conflict, and nothing recorded, when the
    // room is not a stream's chat (is_stream_chat), lists members, is recorded as another kind,
    // or was created on the room plane as another kind (whose kind and delivery are fixed when it
    // is created); recording it again does nothing. The in-memory store keeps no room plane, so
    // only the first three apply to it.
    virtual void record_live(const RoomId& room, MessageCallback<void> done) = 0;
    // Where changes to any room's member list are told from now on; nullptr stops them. One
    // listener at a time.
    virtual void watch_members(IMemberListener* listener) noexcept = 0;

    // Member lists as their users change them (ADR-0096). Each decides from the list as it is when
    // it runs, under the room's lock, so two changes of one room never both pass a check that
    // only one of them would: authority, the size cap and the room's kind are read with the
    // write. Ids in `changed` are those the change actually made.
    //
    // The direct chat of `user` and `peer` (distinct), at `room` (named_kind DirectChat): records
    // it and lists both when it lists nobody yet; otherwise changes nothing, so a member an
    // operator removed is not put back. NotMember when it lists others but not `user`, WrongKind
    // when it is recorded as another kind, RoomLimit when it would list it for a `user` already
    // in kMaxRoomsPerUser rooms.
    virtual void open_direct(const RoomId& room, const UserId& user, const UserId& peer,
                             MessageCallback<MembershipChange> done) = 0;
    // A group chat at `room` (named_kind GroupChat), `creator` its admin and `members` (at most
    // kMaxMembersPerChange, distinct, without the creator) its members: when it lists nobody yet.
    // A repeat changes nothing and answers Done if the creator is still listed, NotMember if not;
    // WrongKind when the room is recorded as another kind; RoomLimit as for open_direct; Gone
    // when it lists nobody but holds messages.
    virtual void create_group(const RoomId& room, const UserId& creator,
                              std::vector<UserId> members,
                              MessageCallback<MembershipChange> done) = 0;
    // `users` (at most kMaxMembersPerChange, distinct) listed in the group chat by its admin
    // `actor`, or by the service; those already listed are left as they are. Full when the group
    // would pass kMaxGroupMembers, and then nobody is added. For the service, NotMember means the
    // room is not recorded at all.
    virtual void add_members(const RoomId& room, const Actor& actor, std::vector<UserId> users,
                             MessageCallback<MembershipChange> done) = 0;
    // `user` taken off the group chat's list by its admin `actor`; someone not listed changes
    // nothing. `actor` itself leaves as leave_room does.
    virtual void expel(const RoomId& room, const UserId& actor, const UserId& user,
                       MessageCallback<MembershipChange> done) = 0;
    // `user` leaves the group chat. When the last admin goes and anyone is left, the remaining
    // member whose id sorts first becomes admin. NotGroup for a direct chat, whose list does
    // not change.
    virtual void leave_room(const RoomId& room, const UserId& user,
                            MessageCallback<MembershipChange> done) = 0;
    // The rooms `user` is listed in, in byte order of their ids from after `after`, at most
    // min(limit, kMaxListPage + 1).
    virtual void rooms_of(const UserId& user, std::optional<RoomId> after, std::size_t limit,
                          MessageCallback<std::vector<RoomEntry>> done) = 0;
    // The room's members and their roles, in byte order of their ids from after `after`, at most
    // min(limit, kMaxListPage + 1): only for `asker` while listed, and always for the service
    // (which reads as listed).
    virtual void roster(const RoomId& room, const Actor& asker, std::optional<UserId> after,
                        std::size_t limit, MessageCallback<Roster> done) = 0;
    // Which of `others` (at most kMaxSharedAsked) share a direct or group chat with `user` now,
    // each once and in byte order of their ids: whose presence `user` may see (ADR-0056 as
    // amended by ADR-0096). A stream's live chat lists nobody and shares nothing; `user` is never
    // in the answer.
    virtual void shared_with(const UserId& user, std::vector<UserId> others,
                             MessageCallback<std::vector<UserId>> done) = 0;
};

} // namespace core::ports
