#pragma once

#include "sql.hpp"

#include <string_view>

// The message store's statements, apart from its operations so that tests can EXPLAIN exactly
// what it runs. The history pages keep their text as views, whose length is known at compile
// time, for that.
namespace infra::postgres::message_sql {

// Both pages walk the primary key from the cursor and stop at the row limit; the running sum
// then cuts the page where its bodies pass the byte bound. The first row is kept whatever its
// size (its running sum is its own length): no writer here stores a body over the bound, but a
// page that came back empty would read as the end of the room. The window reads rows in index
// order and one row ahead of what it emits, so the limit stops the scan early.
// $1 room, $2 cursor (exclusive), $3 row limit, $4 byte bound.
inline constexpr std::string_view kHistoryBeforeText = R"sql(
SELECT seq, sender, msg_key, (extract(epoch FROM sent_at) * 1000000)::bigint, body
  FROM (SELECT seq, sender, msg_key, sent_at, body,
               sum(octet_length(body)) OVER (ORDER BY seq DESC) AS running
          FROM chat_messages
         WHERE room_id = $1 AND seq < $2
         ORDER BY seq DESC
         LIMIT $3) AS page
 WHERE running <= $4 OR running = octet_length(body)
 ORDER BY seq DESC)sql";
// A view of a literal: data() is NUL-terminated.
// NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
inline constexpr Sql kHistoryBefore = kHistoryBeforeText.data();

inline constexpr std::string_view kHistoryAfterText = R"sql(
SELECT seq, sender, msg_key, (extract(epoch FROM sent_at) * 1000000)::bigint, body
  FROM (SELECT seq, sender, msg_key, sent_at, body,
               sum(octet_length(body)) OVER (ORDER BY seq) AS running
          FROM chat_messages
         WHERE room_id = $1 AND seq > $2
         ORDER BY seq
         LIMIT $3) AS page
 WHERE running <= $4 OR running = octet_length(body)
 ORDER BY seq)sql";
// NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
inline constexpr Sql kHistoryAfter = kHistoryAfterText.data();

// The counter the owner's fenced write advances, not max(seq): one source for both.
inline constexpr Sql kLastSeq =
    "SELECT coalesce((SELECT last_seq FROM room_state WHERE room_id = $1), 0)";

// A room with no kind recorded is recorded as a group chat in the same statement, before the
// member row: a concurrent record_live then waits on the chat_rooms key and finds it closed.
// DO NOTHING waits for a concurrent insert of that key as well, without locking a row that is
// already there.
inline constexpr Sql kAddMember = R"sql(
WITH closed AS (
    INSERT INTO chat_rooms (room_id, kind) VALUES ($1, 'group_chat')
    ON CONFLICT (room_id) DO NOTHING)
INSERT INTO chat_members (room_id, user_id) VALUES ($1, $2)
ON CONFLICT (room_id, user_id) DO NOTHING)sql";

inline constexpr Sql kRemoveMember = "DELETE FROM chat_members WHERE room_id = $1 AND user_id = $2";

// Answers with the room's recorded kind and whether $2 is a member. A join reads first, so that
// joining a recorded room takes no row lock and writes nothing. Only a room with no kind recorded
// is written, and only with a closed kind ($3): a join that asks for 'stream_live_chat' records
// nothing and reads a null kind. The update that does nothing on a conflict waits for a
// concurrent first join and returns the kind it recorded, rather than missing it in this
// statement's snapshot. Each probe is one primary-key lookup. $4 is false when the room is not to
// be recorded (Recording::Skipped): the kind comes back null, and the join is answered by the
// kind it asked for.
// A stream's chat that has closed (closed_at, migration 0012) reads as a closed room with no
// members: its kind is still recorded, so nothing is written, and a join is refused.
inline constexpr Sql kAdmits = R"sql(
WITH recorded AS (
    SELECT CASE WHEN closed_at IS NULL THEN kind ELSE 'group_chat' END AS kind
      FROM chat_rooms WHERE room_id = $1),
created AS (
    INSERT INTO chat_rooms (room_id, kind)
    SELECT $1, $3
     WHERE $3 <> 'stream_live_chat' AND $4 AND NOT EXISTS (SELECT 1 FROM recorded)
    ON CONFLICT (room_id) DO UPDATE SET kind = chat_rooms.kind
    RETURNING kind)
SELECT coalesce((SELECT kind FROM recorded), (SELECT kind FROM created)),
       EXISTS (SELECT 1 FROM chat_members WHERE room_id = $1 AND user_id = $2))sql";

// The room's recorded kind (NULL when none is; a closed stream's chat as closed, as kAdmits
// reads it) and whether the user is listed: nothing written.
inline constexpr Sql kAccess = R"sql(
SELECT (SELECT CASE WHEN closed_at IS NULL THEN kind ELSE 'group_chat' END
          FROM chat_rooms WHERE room_id = $1),
       EXISTS (SELECT 1 FROM chat_members WHERE room_id = $1 AND user_id = $2))sql";

// Records the room as open unless its id is not a stream's (version 8, tagged 0x01 in its first
// byte, ADR-0070), it lists
// members, or the room plane already created it as another kind, and answers with the kind it is
// recorded as: 'stream_live_chat' when it is open, another kind or no row when it is not.
// room_state's kind and delivery are copied when the room is created and never change, so opening a
// room created closed would leave it durable and group_chat there while chat_rooms said live. A
// room is recorded closed before its first member row (kAddMember), so a member insert in flight
// holds the chat_rooms key this waits on, and the kind it then returns is the closed one.
inline constexpr Sql kRecordLive = R"sql(
INSERT INTO chat_rooms (room_id, kind)
SELECT $1, 'stream_live_chat'
 WHERE get_byte(uuid_send($1), 6) >> 4 = 8 AND get_byte(uuid_send($1), 0) = 1
   AND NOT EXISTS (SELECT 1 FROM chat_members WHERE room_id = $1)
   AND NOT EXISTS (SELECT 1 FROM room_state
                    WHERE room_id = $1 AND kind <> 'stream_live_chat')
ON CONFLICT (room_id) DO UPDATE SET kind = chat_rooms.kind
RETURNING kind)sql";

// '' sorts before every id, none of which is empty.
inline constexpr Sql kMembers = R"sql(
SELECT user_id FROM chat_members
 WHERE room_id = $1 AND user_id > $2
 ORDER BY user_id
 LIMIT $3)sql";

// Member lists their users change (ADR-0096). Each change runs in a transaction: BEGIN, then for a
// change that may create a room kLockCreator and kEnsureRoom, then kLockRoom, which takes the
// room's chat_rooms row FOR UPDATE, then the change's own statement, then COMMIT. Under READ
// COMMITTED every statement takes a fresh snapshot, so the change reads the list as the previous
// holder of the lock committed it: two adds cannot both pass the size cap, and an admin removed a
// moment ago cannot add anyone. Everything a change checks it checks inside its own statement, and
// writes only when the checks pass, so a refusal writes nothing and the transaction commits empty.
// The lock is only ever taken on one room at a time, so these transactions cannot deadlock one
// another. Users travel as one text of ids separated by spaces, which no id contains.

// Records the room as $2 when nothing recorded it. DO NOTHING: a kind already there stays, and
// the lock below reads it.
inline constexpr Sql kEnsureRoom = R"sql(
INSERT INTO chat_rooms (room_id, kind) VALUES ($1, $2)
ON CONFLICT (room_id) DO NOTHING)sql";

// The room's recorded kind, its row locked until the transaction ends; no row when none is.
inline constexpr Sql kLockRoom = "SELECT kind FROM chat_rooms WHERE room_id = $1 FOR UPDATE";

// Taken by a change that may create a room, before the room's own lock: serialises the changes
// one user makes on every node, so that the count of rooms they are listed in (kOpenDirect,
// kCreateGroup) cannot be passed by two creates at once. Key space 4 of the two-int advisory
// locks; the directory's user lock is 3 (e2ee_directory.cpp). Only these changes take it, and
// always first, so it never waits behind a room lock its holder needs.
inline constexpr Sql kLockCreator = "SELECT pg_advisory_xact_lock(4, hashtext($1))";

// A direct chat lists its pair when it lists nobody yet: when it was just recorded, or a join
// recorded it before anyone opened it, and the user asking is listed in fewer than $4 rooms. A
// room that lists anyone is left as it is, so a member an operator removed is not put back. $1
// room, $2 the user asking, $3 the other. Answers whether $2 is listed now, who was added,
// whether the room listed nobody, and whether $2 had room for one more.
inline constexpr Sql kOpenDirect = R"sql(
WITH listed AS (SELECT user_id FROM chat_members WHERE room_id = $1),
fits AS (
    SELECT (SELECT count(*) FROM (SELECT 1 FROM chat_members WHERE user_id = $2 LIMIT $4) AS c)
           < $4 AS ok),
added AS (
    INSERT INTO chat_members (room_id, user_id, role)
    SELECT $1, u, 'member' FROM unnest(ARRAY[$2, $3]) AS u
     WHERE NOT EXISTS (SELECT 1 FROM listed) AND (SELECT ok FROM fits)
    ON CONFLICT (room_id, user_id) DO NOTHING
    RETURNING user_id)
SELECT EXISTS (SELECT 1 FROM listed WHERE user_id = $2)
           OR EXISTS (SELECT 1 FROM added WHERE user_id = $2),
       (SELECT string_agg(user_id, ' ' ORDER BY user_id) FROM added),
       NOT EXISTS (SELECT 1 FROM listed),
       (SELECT ok FROM fits),
       false)sql";

// A group chat lists its creator as admin and the others as members when it lists nobody yet,
// holds no message, and the creator is listed in fewer than $4 rooms; a repeat of the create
// changes nothing. A group whose members all left keeps its history, which a create under the
// same id must not hand to whoever it lists. $1 room, $2 creator, $3 the others. Answers as
// kOpenDirect, and whether the room holds messages.
inline constexpr Sql kCreateGroup = R"sql(
WITH listed AS (SELECT user_id FROM chat_members WHERE room_id = $1),
fits AS (
    SELECT (SELECT count(*) FROM (SELECT 1 FROM chat_members WHERE user_id = $2 LIMIT $4) AS c)
           < $4 AS ok),
used AS (SELECT EXISTS (SELECT 1 FROM chat_messages WHERE room_id = $1) AS held),
added AS (
    INSERT INTO chat_members (room_id, user_id, role)
    SELECT $1, u, CASE WHEN u = $2 THEN 'admin' ELSE 'member' END
      FROM unnest(array_prepend($2, string_to_array($3, ' '))) AS u
     WHERE NOT EXISTS (SELECT 1 FROM listed) AND (SELECT ok FROM fits)
       AND NOT (SELECT held FROM used)
    ON CONFLICT (room_id, user_id) DO NOTHING
    RETURNING user_id)
SELECT EXISTS (SELECT 1 FROM listed WHERE user_id = $2)
           OR EXISTS (SELECT 1 FROM added WHERE user_id = $2),
       (SELECT string_agg(user_id, ' ' ORDER BY user_id) FROM added),
       NOT EXISTS (SELECT 1 FROM listed),
       (SELECT ok FROM fits),
       (SELECT held FROM used))sql";

// $3's users not listed yet join the group, if $5 (the room is a group chat), $2 is its admin
// and the list stays within $4. Answers $2's role (NULL: not listed), whether the list fits, and
// who was added.
inline constexpr Sql kAddMembers = R"sql(
WITH actor AS (SELECT role FROM chat_members WHERE room_id = $1 AND user_id = $2),
fresh AS (
    SELECT DISTINCT u FROM unnest(string_to_array($3, ' ')) AS u
     WHERE NOT EXISTS (SELECT 1 FROM chat_members WHERE room_id = $1 AND user_id = u)),
fits AS (
    SELECT (SELECT count(*) FROM chat_members WHERE room_id = $1)
           + (SELECT count(*) FROM fresh) <= $4 AS ok),
added AS (
    INSERT INTO chat_members (room_id, user_id, role)
    SELECT $1, u, 'member' FROM fresh
     WHERE $5 AND (SELECT role FROM actor) = 'admin' AND (SELECT ok FROM fits)
    ON CONFLICT (room_id, user_id) DO NOTHING
    RETURNING user_id)
SELECT (SELECT role FROM actor), (SELECT ok FROM fits),
       (SELECT string_agg(user_id, ' ' ORDER BY user_id) FROM added))sql";

// $3 leaves the group's list, if $4 (the room is a group chat) and $2 is its admin. $2 is not
// $3 (that is a leave), and stays an admin, so nobody needs promoting. Answers $2's role and
// who was removed.
inline constexpr Sql kExpel = R"sql(
WITH actor AS (SELECT role FROM chat_members WHERE room_id = $1 AND user_id = $2),
gone AS (
    DELETE FROM chat_members
     WHERE room_id = $1 AND user_id = $3 AND $4 AND (SELECT role FROM actor) = 'admin'
    RETURNING user_id)
SELECT (SELECT role FROM actor), (SELECT user_id FROM gone))sql";

// $2 leaves, if $3 (the room is a group chat). When $2 was the last admin, the member left whose
// id sorts first becomes one: a group always has an admin while it has members. Answers whether
// $2 was listed, and who was promoted.
inline constexpr Sql kLeave = R"sql(
WITH listed AS (
    SELECT role FROM chat_members WHERE room_id = $1 AND user_id = $2),
gone AS (
    DELETE FROM chat_members WHERE room_id = $1 AND user_id = $2 AND $3
    RETURNING role),
heir AS (
    SELECT user_id FROM chat_members
     WHERE room_id = $1 AND user_id <> $2
       AND (SELECT role FROM gone) = 'admin'
       AND NOT EXISTS (SELECT 1 FROM chat_members
                        WHERE room_id = $1 AND user_id <> $2 AND role = 'admin')
     ORDER BY user_id
     LIMIT 1),
promoted AS (
    UPDATE chat_members SET role = 'admin'
     WHERE room_id = $1 AND user_id = (SELECT user_id FROM heir)
    RETURNING user_id)
SELECT EXISTS (SELECT 1 FROM listed), (SELECT user_id FROM promoted))sql";

// A user's rooms by room id, through chat_members_by_user (0015), with each room's kind (NULL
// when none is recorded) and, for a direct chat, the other member listed. The first page has no
// cursor. $1 user, $2 row limit, $3 the room to page after. A view, as the history pages are,
// so that a test can EXPLAIN it.
inline constexpr std::string_view kRoomsFirstText = R"sql(
SELECT m.room_id, r.kind, m.role,
       CASE WHEN r.kind = 'direct_chat' THEN
           (SELECT o.user_id FROM chat_members o
             WHERE o.room_id = m.room_id AND o.user_id <> $1 ORDER BY o.user_id LIMIT 1)
       END
  FROM chat_members m LEFT JOIN chat_rooms r ON r.room_id = m.room_id
 WHERE m.user_id = $1
 ORDER BY m.room_id
 LIMIT $2)sql";
// NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
inline constexpr Sql kRoomsFirst = kRoomsFirstText.data();

inline constexpr Sql kRoomsAfter = R"sql(
SELECT m.room_id, r.kind, m.role,
       CASE WHEN r.kind = 'direct_chat' THEN
           (SELECT o.user_id FROM chat_members o
             WHERE o.room_id = m.room_id AND o.user_id <> $1 ORDER BY o.user_id LIMIT 1)
       END
  FROM chat_members m LEFT JOIN chat_rooms r ON r.room_id = m.room_id
 WHERE m.user_id = $1 AND m.room_id > $3
 ORDER BY m.room_id
 LIMIT $2)sql";

// A page of the room's members and roles, for $2 only while listed. Always one row at least:
// the first column says whether $2 is listed, and a page with nobody (past the end, or for
// someone not listed) is one row of NULLs beside it. $3 the id to page after ('' sorts before
// every id), $4 the row limit.
inline constexpr Sql kRoster = R"sql(
SELECT a.listed, p.user_id, p.role
  FROM (SELECT EXISTS (SELECT 1 FROM chat_members WHERE room_id = $1 AND user_id = $2) AS listed) a
  LEFT JOIN LATERAL (
       SELECT user_id, role FROM chat_members
        WHERE room_id = $1 AND user_id > $3
        ORDER BY user_id
        LIMIT $4) p ON a.listed
 ORDER BY p.user_id)sql";

} // namespace infra::postgres::message_sql
