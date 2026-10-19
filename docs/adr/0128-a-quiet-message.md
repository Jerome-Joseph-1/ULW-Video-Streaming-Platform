# 0128. A quiet message

Status: Accepted
Date: 2026-10-19
Amends: ADR-0112 (the node channel moves to version 7)

## Context

Reactions, edits and delete markers were sent as ordinary messages. Each one moved the room's
seq, so `joined.seq`, the only unread signal, moved too. Each one made a hook `message` event,
so a 👍 pushed "New message from …" (#212).

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| A `quiet` flag on `send`, kept with the message, on its frames, skipped by the hook, and `loud_seq` on `joined` | Quiet messages stay ordered and stored as any message; clients count unread from `loud_seq` | A flag on the node channel (version 7) and a column | Accepted |
| Leave it to clients' bodies | Nothing for chat to do | The hook cannot see bodies; `joined.seq` stays the only server signal | Rejected |
| An unsequenced side channel for reactions | No seq bump | Lost on reconnect, never in history, out of order with the messages they react to | Rejected |

## Decision

- `send` takes an optional `quiet` (default `false`). It travels with the message as a flags
  byte on the node channel's Send and Deliver (version 7), and is kept in `chat_messages.quiet`
  (migration 0025, a constant default, so no row is rewritten).
- A quiet message is sequenced, stored, delivered, resumed and paged in history like any other,
  with `quiet: true` on its frames. The sender's node makes no hook `message` event for it.
- `joined` carries `loud_seq`: the newest message that is not quiet. The node reads it from the
  store when it first enters the room, holding the join's answer until it has. After that every
  loud delivery moves it. It is absent if the store cannot say, and for a stream's live chat.

## Consequences

- An unread badge counts against `loud_seq`, and reactions neither raise it nor push.
- Entering a room on a node costs one more indexed read.
- The node channel moves from 6 to 7: roll the release out with `Recreate`, as for any version
  change (RUNBOOK section 3). Both changes ship in this release, so one `Recreate` covers them.
