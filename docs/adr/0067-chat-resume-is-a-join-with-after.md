# 0067. Chat resume is a `join` with `after`, and acks are `joined` and `sent`

Status: Accepted
Date: 2026-09-29

## Context

Brief section 3 left the chat protocol's join, leave and ack shapes and its resume protocol
(`resume_from_seq`) open. ADR-0036 fixed the M16 envelope (`join`, `send`, `joined`, `sent`,
`message`, `error`) and deferred acks and resume to M17. ADR-0043 built them and describes the
resume budgets, but its text says "resume" in passing and does not record that the request field
is not the name the brief used, nor what happened to leave. Client authors need one place to
read the shapes.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| A separate `resume` command with `resume_from_seq` | Matches the brief's name; resume is explicit | Rejected: a rejoin and a resume are the same act, and a second command would need its own room-membership rules |
| A field on `join` | One command; a fresh join and a reconnect differ by one field | Accepted |
| A `leave` command | Symmetric | Not built: see the decision |
| Per-message acks from the client (`ack`) | Would let the server drop what was received | Not built: the server keeps a window of recent messages, not a per-client cursor |

## Decision

The envelope is the one in `apps/chat/src/envelope.hpp` (ADR-0036, ADR-0043):

- **Resume.** `{"type":"join","room":"<uuid>","after":N}`. After `joined`, the node sends the
  messages it still holds above seq N, oldest first, as many of the newest as fit the replay
  budget (128 KiB, per client per linger), then live messages. `after` is an unsigned integer;
  a negative or non-integer value is `malformed`, as is an unknown field. Without `after` the
  join sends nothing old. The budgets and the cost of a repeated resume are ADR-0043's.
- **Where the client resumes from.** The client's own last seen `seq`. Seqs of a room rise by
  one per message, so a jump is a gap that the client fills from history (M19), not from this
  protocol. `joined` carries `seq`, the room's latest seq known to the node, so a gap is visible
  even when no message follows.
- **Acks.** `sent` is the ack of a `send`: `{"type":"sent","room":...,"id":<message id>,
  "seq":N}`, where `id` is the sender's message id and `seq` the room's `last_seq` after that
  append. A repeat of an id gets the first send's seq. `joined` is the ack of a `join`. There is
  no client-to-server ack of received messages.
- **Leave.** There is no `leave` command. A `type` other than `join` or `send` is refused as
  `malformed`; `envelope_test.cpp` pins `leave` among them. A connection leaves its rooms by closing; the node leaves a room on the room
  plane once no client has used it for 30 s (`linger`, ADR-0043). A client holds at most 64
  rooms per connection and can only shed one by reconnecting (`too_many_rooms`).

## Consequences

- One command carries join and resume, and `after: 0` asks for everything the node holds.
- Divergence from the brief, plainly: the brief's `resume_from_seq` does not exist. The field is
  `after`, an exclusive lower bound (messages with a seq greater than N), on `join`, in
  `apps/chat/src/envelope.cpp` and `docs/integration/chat.md`. There is no leave message and no
  client ack; the shapes above are the whole protocol.
- Kept messages live on the node that delivered them. A client that reconnects through another
  node, or after the room lingered, gets what that node has and sees the rest as a gap.
- If a client-side leave is needed (freeing one of the 64 rooms without reconnecting), it is a
  new command and a new ADR; it would have to drop the room from the node's subscriber list and
  release the room after `linger`.
