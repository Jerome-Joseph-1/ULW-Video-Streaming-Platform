# 0039. A message is stored with its seq, in one statement, before it is delivered

Status: Accepted
Date: 2026-09-29

## Context

A room's owner gives every message a sequence number from `room_state.last_seq` (ADR-0015) and
sends it to the room's members (ADR-0035). Nothing keeps the message itself: a client that
reconnects cannot catch up, and a restart loses the room's history. Section 8.15 asks for
`chat_messages(room_id, seq, sender, body bytea, sent_at)` keyed by `(room_id, seq)`, a
membership table, and history paged by `(room_id, seq DESC)`. Bodies are opaque bytes end to
end, and once end-to-end encryption lands (ADR-0016) they are MLS ciphertext and commits: the
store must not change for that, and neither may the chat service or the router (M22).

MLS adds a constraint on where the write goes. Members apply commits in seq order, filling any
gap from history before applying a later one. That is sound only if every seq the owner takes
is either stored or never existed: a seq taken, delivered to some members and lost with its
owner is a hole that history can never fill, and members on either side of it apply different
commits for the same epoch.

## Options

Where the message is written, relative to taking its seq:

| Option | Why it was tempting | Verdict |
|---|---|---|
| Take the seq and insert the message in one statement: the fenced `UPDATE room_state ... RETURNING last_seq` feeds the `INSERT` | One round trip and one commit; a seq exists only with its row, and a fenced writer takes neither | Accepted |
| Take the seq, then insert the message as a second statement, then deliver | The message store stays apart from the room store | Rejected: two commits in series for every message, and an owner that stops between them leaves a seq with no row |
| Take the seq, deliver, and write behind in batches | One commit per batch; delivery latency unchanged | Rejected: a crash between delivery and flush leaves seqs that members saw and history lacks |
| Batch several messages of a room into one statement | Fewer commits under load | Rejected: an owner writes a room's messages one at a time (ADR-0035), so each would also wait for the batch window |

What bounds a page of history:

| Option | Why it was tempting | Verdict |
|---|---|---|
| Rows only | One `LIMIT` | Rejected: 256 bodies of 64 KiB are 16 MiB for one client, whose unsent output is capped at 256 KiB (ADR-0036) |
| Rows and bytes: stop at 256 rows or when the bodies reach 256 KiB | Bounded by what a client connection may hold; a page always makes progress, since one body (64 KiB) is below the byte bound | Accepted |

## Decision

- **The owner's write.** `PgRoomStore::append_message(room, generation, sender, body, sent_at)`
  runs one statement: the fenced increment of `room_state.last_seq` and, from its result, the
  message's `INSERT` into `chat_messages`. It answers the new seq, or nothing when the
  generation is no longer the room's; then no seq was taken and no row written. If the insert
  fails, the statement fails whole and the seq is not taken. It is not idempotent: a repeat
  after a lost answer is a second message under the next seq, as a repeated send is today
  (ADR-0035); clients deduplicate by their own message ids.
- **Every room, durable or lossy, writes this way**, before delivery. Storing the row costs
  nothing measurable over taking the seq alone (below), so there is no cheaper path for lossy
  rooms to keep, and E2EE rooms are always durable.
- **How the router calls it.** The router's store port, `rt::IRoomStore::append(room,
  generation)`, becomes `append(room, generation, sender, body, sent_at)` carrying the
  message, with the same answer; `RoomRegistry::append` passes it through, and the owner's
  pump in `rt/src/room_router.cpp` hands over the write it already holds (sender, body) with
  `sent_at` from its clock. The Postgres implementation is `append_message` above. This changes
  `room_router.cpp` once, in M19, before phase 2 is tagged; after that, encrypted bodies change
  nothing on this path.
- **One writer.** `append_message` is the only statement that writes `chat_messages`. The
  message store below has no writer: a row written under a seq "taken elsewhere" could sit
  above `last_seq` and make every later append for its room fail. The seq-only
  `IRoomStore::append` still exists, because the router calls it today; it takes a seq with no
  row, so rooms written through it have holes in their history. The wiring step removes it,
  with the router change above, so that no path takes a seq without its message.
- **One counter.** `room_state.last_seq` is the only source of a room's last seq. The message
  store's `last_seq(room)` reads it, not `max(seq)` of the messages; with `append_message` as
  the only writer the two agree.
- **Reads.** `core::ports::IMessageStore` (`core/include/core/ports/message_store.hpp`) serves
  history: `history_before(room, before, limit)` newest first, `history_after(room, after,
  limit)` oldest first, `last_seq(room)`, and membership (`add_member`/`remove_member`/
  `members`). Every call answers once, on the reactor thread, never from inside the call.
  Errors are values (`Unavailable`, `Conflict`, `TooLarge`, `Corrupt`); none carries text, so
  none can carry a body. The in-memory store implements the same port, and keeps a writer of
  its own, `append(room, seq, ...)`, for whatever takes seqs in tests and single-process runs:
  it has no counter to take them from.
- **Opaque bodies.** `body` is `bytea NOT NULL`, bound as a binary parameter and read back with
  `encode(body, 'hex')`, so the server's `bytea_output` setting does not matter. Nothing parses,
  collates or indexes it: the only index is the primary key `(room_id, seq)`, and the table has
  no check constraint, because a violated check writes the failing row, body and all, into the
  error and the server log. The body bound, 64 KiB (`kMaxMessageBody`), is the WebSocket
  decoder's message bound (ADR-0029), checked before anything is sent. MLS ciphertext and
  commits are bodies like any other.
- **Pages.** At most `min(limit, 256)` rows whose bodies add up to at most 256 KiB. The query
  walks the primary key from the cursor, forwards or backwards, under a `LIMIT`; a running
  `sum(octet_length(body))` window cuts the page at the byte bound. A backward scan of
  `(room_id, seq)` is the `(room_id, seq DESC)` order, so no second index exists.
- **Membership** is `chat_members(room_id, user_id)`, `user_id` in the `"C"` collation, so ids
  page in byte order whatever the database's default collation; the in-memory store orders them
  the same way.
- **Sessions.** The message store drives its own pool of four sessions on the reactor, as the
  room store does, with a 2 s request timeout.
- An in-memory store implements the same port, and one conformance suite
  (`tests/conformance/message_store_conformance_test.cpp`) runs against both.

## Consequences

- Measured on the development machine (Postgres 16 in Docker, `synchronous_commit = on`, the
  host shared with other builds at a load average of 11 to 25 on 4 cores), 300 messages
  written one after another to one room:

  | Write | p50 | p99 |
  |---|---|---|
  | seq and row in one statement (`append_message`) | 1.3-4.0 ms | 5-9 ms |
  | seq alone (`append`, today's path) | 1.3-4.0 ms | 8-10 ms |
  | seq, then the row as a second statement | 2.7-8.0 ms | 11-19 ms |

  A read by primary key on the same pool: p50 0.35-0.4 ms. The commit's WAL flush is almost all
  of a write, so storing the message in the seq's own statement is free, and a second
  statement doubles the cost. A room's ceiling stays where ADR-0035 put it, one write per
  commit.
- Paging a 10,000-message room from newest to oldest, 100 per page, took 108-128 ms (about
  1.1 ms a page). `EXPLAIN ANALYZE` of a page, with four other 10,000-message rooms in the
  table, shows an index scan of `chat_messages_pkey` (backward for older pages, forward for
  newer) reading 101 rows, the page and the window's one row of lookahead, in 0.14-0.2 ms,
  with no sort.
- Bodies bound as parameters stay out of statement text, `pg_stat_activity` and the server's
  error log. Parameters are still written to the server log if it runs with `log_statement =
  all` (or `mod`), or logs slow statements, unless `log_parameter_max_length` is 0; production
  Postgres must keep one of those off. A test writes a known plaintext and finds it in no log.
- The history endpoint that serves these pages to clients, and the join answer that tells a
  client the room's head seq so it can see a gap, are the chat service's.
- History is never deleted. Retention, and what a user's deletion request removes, are open.
- Nothing partitions `chat_messages` yet. Reopen when maintenance or vacuum of it becomes
  noticeable.
