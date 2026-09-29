# 0052. A message is stored with its seq, in one statement, before it is delivered

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

- **The owner's write.** `PgRoomStore::append(room, generation, const rt::Outgoing&
  {sender, key, body})`, the router's store port, runs one statement: the fenced increment of
  `room_state.last_seq` and, from its result, the message's `INSERT` into `chat_messages`. It
  answers the new seq, or nothing when the generation is no longer the room's; then no seq was
  taken and no row written. If the insert fails, the statement fails whole and the seq is not
  taken. `sent_at` is the database's `now()` at the write, so the router passes no time.
- **An oversized body is refused, as `Unavailable`.** A body over 64 KiB is answered without
  a statement: the client edge decodes nothing larger, the node channel refuses any frame whose
  body is larger (`rt/src/wire.cpp`), and input a peer controls must never stop the owner.
  `rt::StoreError` gains no `TooLarge` for it: no client can send such a body, so the router
  has nothing to tell one, and the store port stays as the router uses it. To the router it is
  a write that did not happen.
- **Repeats are recognised by the sender's message key.** A client that got `unavailable`
  sends the same message again under the same key, to the same owner or, after a takeover, to
  the next one; either may have no memory of it, since a lost answer means it was never fanned
  out. So the key is stored with the message (`msg_key`, unique per room and sender), and
  the append first looks for it: found, the statement takes no seq and answers the one
  the message was stored under; the `room_state` row is still updated by nothing, so a fenced
  former owner gets no answer for it either. Two repeats in flight at once both miss the
  lookup; the second fails on the key's unique index, whole, and is run once more, when it
  finds the first. The index holds the room, sender and key, never the body.
- **A key reused for another body is a conflict.** The lookup compares the stored body with
  the new one: the same bytes are a repeat, answered with its seq; other bytes take no seq,
  write nothing, and are answered `StoreError::Conflict`, which reaches the client as
  `conflict` and is delivered to nobody. Answering the old seq instead would have the owner fan
  out the new body under a seq whose stored body is the old one, so that history and what
  members saw disagree. The router's cache of recent keys keeps a digest of each body for the
  same reason, and gives the same answer without asking the store. The digest is FNV-1a,
  which is not cryptographic: a sender can make two bodies collide, but the cache is keyed by
  sender and key, so a collision can only have the sender's own resend of their own key
  answered as a repeat, which the store, comparing the bytes, would have refused. Reply status `Conflict` is
  new on the node channel, whose version is therefore 3 (ADR-0043's nodes speak 2).
- **A repeat the store recognised is not delivered again.** The answer is a seq the owner has
  already passed, and the owner, seeing it at or below its head, answers the sender and
  delivers nothing. An owner that took the room since, and has not yet learnt the room's head,
  cannot tell, and delivers the stored message once more under its original seq; a client that
  has that seq already drops it.
- **Every room, durable or lossy, writes this way**, before delivery. Storing the row costs
  nothing measurable over taking the seq alone (below), so there is no cheaper path for lossy
  rooms to keep, and E2EE rooms are always durable.
- **How the router calls it.** Unchanged: the owner's pump calls `IRoomStore::append` with
  the message it sequences, as it did before messages were stored, and `RoomRegistry` passes
  it through. Storage changed only the store, so `rt/src/room_router.cpp` does not change for
  it, and encrypted bodies change nothing on this path.
- **One writer.** `PgRoomStore::append` is the only statement that writes `chat_messages`, and
  there is no path that takes a seq without its message: the seq-only append is gone. The
  message store below has no writer: a row written under a seq "taken elsewhere" could sit
  above `last_seq` and make every later append for its room fail.
- **One counter.** `room_state.last_seq` is the only source of a room's last seq. The message
  store's `last_seq(room)` reads it, not `max(seq)` of the messages; with the append as the only
  writer the two agree.
- **Reads.** `core::ports::IMessageStore` (`core/include/core/ports/message_store.hpp`) serves
  history: `history_before(room, before, limit)` newest first, `history_after(room, after,
  limit)` oldest first, `last_seq(room)`, and membership (`add_member`/`remove_member`/
  `members`). Every call answers once, on the reactor thread, never from inside the call.
  Errors are values (`Unavailable`, `Conflict`, `TooLarge`, `Corrupt`); none carries text, so
  none can carry a body. The in-memory store implements the same port, and keeps a writer of
  its own, `append(room, seq, ...)`, for whatever takes seqs in tests and single-process runs:
  it has no counter to take them from.
- **Opaque bodies.** `body` is `bytea NOT NULL`, bound as a binary parameter and read back in
  bytea's hex text form (`bytea_output = hex`, the server default, as the key-package store
  also requires; anything else is read as `Corrupt`, never guessed at). Nothing parses, collates
  or indexes it: the indexes are the primary key `(room_id, seq)` and the key's `(room_id,
  sender, msg_key)`, and the table has no check constraint, because a violated check writes the
  failing row, body and all, into the error and the server log. The body bound, 64 KiB (`kMaxMessageBody`), is the WebSocket
  decoder's message bound (ADR-0029), checked before anything is sent. MLS ciphertext and
  commits are bodies like any other.
- **Pages.** At most `min(limit, 256)` rows whose bodies add up to at most 256 KiB. The query
  walks the primary key from the cursor, forwards or backwards, under a `LIMIT`; a running
  `sum(octet_length(body))` window cuts the page at the byte bound. The first row is kept
  whatever its size: nothing here writes a body over the bound, but a row that came to be
  there anyway must not make an empty page, which reads as the start of the room. A backward scan of
  `(room_id, seq)` is the `(room_id, seq DESC)` order, so no second index exists.
- **Membership** is `chat_members(room_id, user_id)`, `user_id` in the `"C"` collation, so ids
  page in byte order whatever the database's default collation; the in-memory store orders them
  the same way. No client command changes it; it is the operators' (and later the product's) to
  set.
- **Who may be in a room follows from its kind, never from an empty list.** Direct and group
  chats are closed: `admits(room, user, asked)` admits only their members, and a closed room
  with no members admits nobody. A stream's live chat is open to anyone. The kind is recorded
  in `chat_rooms`, once, and never changes.
- **A client can never widen who may be in a room.** A join records a kind only for a room
  with none recorded, and only a closed one: the kind it names (`"kind"` in the envelope,
  `"direct"` or `"group"`; a join that names none asks for a group chat). A join that asks for
  the live kind (a stream join, ADR-0057) in a room not recorded as live is answered
  `Admission::NotLive`, which reaches the client as `not_live`, and records nothing. Recording
  a room as live is a server-side step only: `IMessageStore::record_live(room)`, or the SQL in
  the runbook (section 3), which refuse a room that is not a stream's (ADR-0057), lists members, is recorded as another kind, or was already created on the room
  plane as another kind (`room_state`'s kind and delivery are fixed at creation, so opening such
  a room would leave it durable and `group_chat` there while `chat_rooms` said live). Adding a
  member records an unrecorded room as a group chat in the same statement, before its member
  row, so a member insert in flight holds the `chat_rooms` key that `record_live` waits on: the
  closed record wins, and a room listed before it is opened is never opened. The other order is
  allowed: a member added after a `record_live` committed is listed in a room that stays live.
  That is harmless, because a live room admits anyone and its list admits nobody extra. A join reads the recorded kind first and writes only for an unrecorded
  room, so the joins of a recorded room take no row lock and cause no WAL flush; a first join
  racing another waits for it and reads the kind it recorded. Rooms that existed before this
  ADR are recorded as group chats by migration 0006, and so become closed: until then no join
  was checked. A stream's room among them needs a new room id to be opened. The chat service asks before a client
  joins a room new to its connection, which is before the room plane resolves, and so creates,
  the room. `room_state`'s `kind` (0003), written as `group_chat` for every room until now, is
  copied from the recorded kind when the room is created, and its `delivery` is lossy for a
  live chat and durable otherwise. A room created without a kind recorded first takes the kind
  one function in the room store chooses from the room alone (`kind_of_unrecorded`): a group
  chat, closed. Which kinds admit anyone is one function too (`core::ports::admits_anyone`),
  so a new kind is a case in each, not a new rule.
- **A member removed from the list keeps what they have until they reconnect.** The check runs
  at a join, not per message: a connection already in the room goes on receiving its messages,
  and may read its history, until it closes. Cutting it off at once needs the service to watch
  the list (a notification on removal), which is not built.
- **Sessions.** The message store drives its own pool of four sessions on the reactor, as the
  room store does, with a 2 s request timeout.
- An in-memory store implements the same port, and one conformance suite
  (`tests/conformance/message_store_conformance_test.cpp`) runs against both.
- **History reaches clients through the chat service**, not the router: an envelope command,
  `history`, with a `before` or `after` cursor and a limit of at most 100. The service reads the
  page from the message store and sends it as ordinary `message` frames, then a `history` frame
  with their count; a count of 0 is the end of the room in that direction. The page is cut to
  what the connection can still queue (the 128 KiB a resume may, ADR-0043), and a connection too
  far behind for one message gets `busy`, never an empty page. It is charged to the same
  allowance as joins, since each is a read of the database. Only a connection that joined the
  room may ask, so the member check at the join covers history too. A client that resumes
  through a node that kept nothing sees the head seq in `joined` and fills the gap from
  history; nothing on the node reads the store for it.

## Consequences

- Measured on the development machine (Postgres 16 in Docker, `synchronous_commit = on`, the
  host shared with other builds at a load average of 11 to 25 on 4 cores), 300 messages
  written one after another to one room:

  | Write | p50 | p99 |
  |---|---|---|
  | seq and row in one statement (`append`) | 1.3-4.0 ms | 5-10 ms |
  | seq alone (the seq-only append, since removed) | 1.3-4.0 ms | 8-10 ms |
  | seq, then the row as a second statement | 2.7-8.0 ms | 11-19 ms |

  The last row was measured before the store lost its second writer. With the message key's
  lookup and its unique index, the one statement measured p50 4.0 ms against 4.0 ms for the seq
  alone, at a load average of 5. A read by primary key on the same pool: p50 0.35-0.4 ms. The
  commit's WAL flush is almost all of a write, so storing the message in the seq's own
  statement is free, and a second statement doubles the cost. A room's ceiling stays where
  ADR-0035 put it, one write per commit. The tests report these timings and assert none.
- Paging a 10,000-message room from newest to oldest, 100 per page, took 98-128 ms (about
  1 ms a page). `EXPLAIN ANALYZE` of a page, with four other 10,000-message rooms in the
  table, shows an index scan of `chat_messages_pkey` (backward for older pages, forward for
  newer) reading 101 rows, the page and the window's one row of lookahead, in 0.11-0.2 ms,
  with no sort.
- Bodies bound as parameters stay out of statement text and `pg_stat_activity`, but the server
  still writes bound parameters, bodies included (bytea prints as `\x` and hex), to its log:
  - with every statement it logs: `log_statement = mod` or `all` (an append is an `INSERT`),
    `log_min_duration_statement`, `log_min_duration_sample` or `log_transaction_sample_rate`,
    unless `log_parameter_max_length = 0`;
  - with every plan auto_explain logs, unless `auto_explain.log_parameter_max_length = 0`;
  - with any error in a statement that carries a body, unless
    `log_parameter_max_length_on_error = 0`, which is the default.

  So production Postgres runs with `log_parameter_max_length_on_error = 0`, and with
  `log_parameter_max_length = 0` (and auto_explain's, if it is loaded) whenever any of that
  statement logging is on. The RUNBOOK sets all three on the service's database, and the
  operations contract lists them. Nothing checks them at startup yet; the chat server's
  readiness probe could. A test runs the store's writes, an erroring one included, under those
  settings with every statement logged, and under the same with parameters logged, and
  searches the server's log for the body as text, hex and base64: found only in the second.
- Two repeats of one key in flight at once make the second fail on the key's unique index,
  and the server logs that `ERROR` at its default settings, with a `DETAIL` naming the room,
  the sender and the key; the index holds nothing else, so the body is never in it. The
  statement's parameters stay out of the line under the settings above.
- A violated `NOT NULL` also writes the failing row, body and all, like a check constraint.
  None can be violated: every column is bound from a typed value that has no null (the room's
  uuid, the seq from `room_state`, the sender's id, the key, the body, which binds as an empty
  value when empty, and `now()`).
- A join of a room new to a connection now waits for one primary-key read of the member list,
  as well as for the room's owner. Joins are rate limited per user, so this is not per message.
- History is never deleted. Retention, and what a user's deletion request removes, are open.
- Nothing partitions `chat_messages` yet. Reopen when maintenance or vacuum of it becomes
  noticeable.
