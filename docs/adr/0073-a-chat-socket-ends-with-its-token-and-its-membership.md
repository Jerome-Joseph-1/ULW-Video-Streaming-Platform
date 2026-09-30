# 0073. A chat socket ends with its token, and a room with its membership

Status: Accepted
Date: 2026-09-30

## Context

A chat WebSocket is authenticated once, at the upgrade (ADR-0018's token, checked in
`Session::authenticate`), and a room's member list is read once, at the join (ADR-0054). Both
then held for as long as the socket lived, and a socket lives for as long as its client answers
pings. So a user whose tokens Askedin had stopped issuing (signed out, banned) kept receiving
every room they had joined, and a member taken off a room's list in the database kept receiving
that room and reading its history, until the connection happened to close. The security review
of 2026-09-30 (finding 3) raised both.

Member lists are changed in the database, by the product or an operator: no chat command changes
one, and nothing told a chat node that a row went.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Close the socket at the token's `exp`, on the session's own timer, with a close code the client reads as "reconnect with a fresh token" | One deadline per session on the timer it already re-arms; no work per message; the client already reconnects for 1001 | Accepted |
| An in-band `reauth` command carrying a fresh token | The socket survives a token refresh | Rejected for now: a new command and state machine for a reconnect that costs one handshake an hour |
| Check `exp` on every command and delivery | No timer | Rejected: a check per message for a deadline known at the upgrade, and a client that only listens would never be checked |
| A trigger on `chat_members` that notifies every node of a removal (`LISTEN chat_member_removed`), each node taking the user's sockets out of the room | The database is where lists change, whoever changes them; every node hears it at once, whichever node owns the room, so nothing crosses the node channel | Accepted |
| Re-read the member list on every delivery, or periodically | No new path | Rejected: a query per message per member, or a window of minutes |
| A `remove_member` command that notifies through the room's owner over the node channel | Stays inside the chat plane | Rejected: removals made in the database directly, which is how lists are changed today, would never be heard |

## Decision

- **Token lifetime.** At the upgrade the session notes when its token stops being accepted: its
  `exp` plus the 60 s clock skew every check allows (`core::ports::kTokenClockSkew`), converted
  to the reactor's monotonic clock so a stepped wall clock cannot move it. The session's one
  timer, which already schedules pings, the idle deadline and the stall check, also fires at
  that instant, and the socket is closed with **4001** (in RFC 6455's range for applications)
  and counted in `token_expiries_total`. A client reconnects with a fresh token and resumes
  each room with `join` and `after` (ADR-0067); nothing is lost. There is no check per message.
- **Membership.** Migration 0009 adds a trigger on `chat_members`: every deleted row, and the old
  pair of an updated one, is sent with `pg_notify('chat_member_removed', '<room> <user>')`.
  `PgMessageStore` listens on one more session (`IMessageStore::watch_members`), and the chat
  service takes each of that user's clients on the node out of the room: its subscription, its
  place waiting for the room plane, and whatever lossy catch-up it was owed. The client gets an
  `error` with `not_member` for the room, unasked, and a history page already on its way is
  dropped. A join still waiting for the member list when the removal arrives is refused
  whatever the list said, since it may have been read before the delete committed. A stream's
  live chat admits anyone, list or not, and is left alone.
- **Missed notifications.** Postgres keeps no notifications for a session that is not listening.
  When the listening session (re)connects, the service checks every closed room each of its
  clients is in against the member list again, one primary-key read per room and user, and
  removes those no longer listed. That is at most 64 reads per connection, so at most 81,920
  per node (1280 connections of 64 rooms), fewer where users share rooms: each room and user
  is read once, however many of the user's sockets are in it. The reads go four at a time, the
  message store's pool, so joins and history reads queue behind four at most, and a node at
  its ceiling finishes in seconds. A read that fails (the database is still partitioned, the
  pool backing off) is asked again a second later, with the rest held until then, so no
  removal is lost to the outage that caused the resync. A read that fails otherwise (a row the
  store cannot read) would fail the same way again: it is logged, counted in
  `member_check_failures_total`, and settled on the safe side, the user taken out of the room.
  A resync that comes while an earlier one's reads are still queued keeps them where they are
  and adds behind them every room and user not queued already, those already asked included,
  since they may have been read before the removal it is for; so a listening session that
  flaps never starves the rooms at the tail. At most one
  read per room of each client is queued (connections times rooms per client). A join still waiting for its member
  list when the resync comes may have read it before a removal that went unannounced: it is
  read again once the join is let in. This happens only when the listening session was lost.
- Removals are counted in `member_removals_total`.

## Consequences

- Clients must handle close code 4001 by fetching a fresh token and reconnecting; a client that
  reconnects with the same expired token is refused with 401 at the upgrade. Each socket lives at
  most one token lifetime plus a minute.
- A member removal reaches every node within one notification round trip. A removal and a
  re-add in quick succession leave the user out of the room until they join again.
- Each chat node holds one more Postgres session (4 + 1 + 5).
- Adding a member needs no notification: a new member joins, which reads the list.
- Presence rooms are not covered: who may watch whom is its own open question (review finding 4).
