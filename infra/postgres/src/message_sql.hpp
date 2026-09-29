#pragma once

#include "sql.hpp"

#include <string_view>

// The message store's statements, apart from its operations so that tests can EXPLAIN exactly
// what it runs. The history pages keep their text as views, whose length is known at compile
// time, for that.
namespace infra::postgres::message_sql {

// Both pages walk the primary key from the cursor and stop at the row limit; the running sum
// then cuts the page where its bodies pass the byte bound. The window reads rows in index
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
 WHERE running <= $4
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
 WHERE running <= $4
 ORDER BY seq)sql";
// NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
inline constexpr Sql kHistoryAfter = kHistoryAfterText.data();

// The counter the owner's fenced write advances, not max(seq): one source for both.
inline constexpr Sql kLastSeq =
    "SELECT coalesce((SELECT last_seq FROM room_state WHERE room_id = $1), 0)";

inline constexpr Sql kAddMember = R"sql(
INSERT INTO chat_members (room_id, user_id) VALUES ($1, $2)
ON CONFLICT (room_id, user_id) DO NOTHING)sql";

inline constexpr Sql kRemoveMember = "DELETE FROM chat_members WHERE room_id = $1 AND user_id = $2";

// '' sorts before every id, none of which is empty.
inline constexpr Sql kMembers = R"sql(
SELECT user_id FROM chat_members
 WHERE room_id = $1 AND user_id > $2
 ORDER BY user_id
 LIMIT $3)sql";

} // namespace infra::postgres::message_sql
