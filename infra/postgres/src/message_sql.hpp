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

} // namespace infra::postgres::message_sql
