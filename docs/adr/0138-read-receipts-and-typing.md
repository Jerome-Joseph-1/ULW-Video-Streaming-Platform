# 0138. Read receipts and typing

Status: Accepted
Date: 2026-10-22

## Context

A client could not show that the other member of a conversation had seen a message, nor that
someone was typing (#206). Chat kept no record of how far anyone had read, and had no way for a
socket to tell the room something that is not a message: a message is sequenced, stored, counted
in `seq`, and fires the notification hook, none of which suits "alice is typing".

The room plane already carries what is not a message: an unsequenced, unstored notice handed to
the room's owner, which passes it to every node with members in the room (`RoomRouter::notify`,
ADR-0091). Its body is opaque to the node channel, so new kinds of notice need no change of the
wire format.

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| Typing as a quiet message | Reaches every joined socket, and history | Stored and sequenced: a row per keystroke burst, and `seq` jumps that are not messages | Rejected |
| Typing as a notice to the room, never stored, one a second per socket | Lossy by nature, as typing is; nothing written; reaches the joined sockets on every node | A notice lost to a change of owner is lost: the next keystroke says it again | Accepted |
| Read marks as a column of `chat_members` | No new table | Every mark writes a new version of the row each join's membership check reads, and dirties its index | Rejected |
| A table of their own, `chat_read_marks`, keyed by room and member, its rows removed with the member by a foreign key | An update changes no indexed column, so it stays heap-only on a page with room left (fillfactor 80); a removed member's mark goes with them, under the same statement | One more table | Accepted |
| Write every read a client sends | Simple | A client that marks each message it shows writes on every message | Rejected |
| Write only a mark past the connection's last, only forward in the statement itself, and at most 10 at once and 2 a second per user | At most one write per message read, and none for a repeat or for a device that read less | A burst past the allowance is refused, and sent again | Accepted |
| Carry marks on `joined` | No extra round trip | A store read on every join, wanted or not | Rejected: `reads` asks for them |

## Decision

- **Typing.** `{"type":"typing","room"}` on a socket that joined a direct or group chat. The node
  hands the room a notice (layout 3: the room and the user); every node with sockets in the room
  pushes `{"type":"typing","room","user"}` to them, except to the typist's own. Never stored. Each
  connection may send 3 at once and one a second after that; more are dropped, unanswered, and
  counted. A stream's live chat refuses it, `not_allowed`.
- **Read marks.** `{"type":"read","room","seq"}` on a socket that joined the room. Migration 0029
  adds `chat_read_marks (room_id, user_id, seq, read_at)`, its key a foreign key to `chat_members`
  `ON DELETE CASCADE`. One statement moves the mark to `seq`, or to the room's latest seq when
  `seq` is past it, only forward and only while the user is listed; a mark at or below what that
  connection already marked is not written at all. A mark that moved is told to the room (layout
  4: room, user, seq), and every joined socket on every node, the reader's own included, hears
  `{"type":"read","room","user","seq"}`. One that did not move tells nobody. Each user may mark
  10 at once and 2 a second, refused `rate_limited` with `retry_after_ms` beyond that.
- **Reading them back.** `{"type":"reads","room"}` on a joined socket, charged as a join, answers
  `{"type":"reads","room","reads":[{"user","seq"}]}`: every member with a mark, in id order, up to
  1024. The service API's `reads` takes `{room}` and answers the same.
- Old nodes count the new notice layouts as malformed and push nothing; nothing else changes for
  them.

## Consequences

- A client renders "Seen" by comparing a message's seq with the other members' marks, from
  `reads` when it opens a room and from `read` frames after that.
- Typing is best effort and needs refreshing: a client shows it for a few seconds after the last
  `typing` frame, and sends `typing` again every few seconds while its user types.
- A mark survives in the database until its member leaves the room, so marks grow with
  memberships, one small row each, not with messages.
- `typing_total{outcome}`, `typing_pushed_total`, `read_marks_total{outcome}`,
  `read_events_total`, `read_listings_total` and `room_notices_malformed_total` count it.
