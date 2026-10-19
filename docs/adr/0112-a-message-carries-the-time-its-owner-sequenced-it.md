# 0112. A message carries the time its room's owner sequenced it

Status: Accepted
Date: 2026-10-19
Amends: ADR-0035 (Deliver carries the message's time; the node channel's version is 6);
ADR-0054 (`sent_at` is the owner's time, not the database's)

## Context

`chat_messages.sent_at` has been stored since ADR-0054, but no `message` frame carried it: live,
resumed and history frames had `room`, `seq`, `sender`, `id` and `body` only. A client could
stamp a message with the time it arrived, which is wrong for everything resumed or loaded from
history, so day separators and "sent at" could not be honest (issue #195).

History reads `sent_at` from the store. A live or resumed message never touches the store on
the way out: the owner fans it out as it is sequenced (ADR-0035), and a member's node keeps it
for resumes (ADR-0056). For the three to agree, the time has to exist before the store write and
travel with the message. `sent_at` was the database's `now()`, which only the database knew.

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| History frames only | No change to the node channel | Live and resumed frames still have no time; a client would see `at` on some messages and not others | Rejected |
| Each member's node stamps the message when it is delivered there | No change to the node channel | Two nodes give one message two times, and history a third; a resumed message would say when it reached the node | Rejected |
| The owner stamps each write before its append, stores that value as `sent_at`, and sends it in `Deliver` | One time per message, the same live, resumed and from history | `Deliver` changes, so the node channel goes to version 6, a `Recreate` rollout | Accepted |

## Decision

- The owner takes the wall clock's time, to the microsecond (the precision `timestamptz`
  keeps), as it starts a write's append (`rt::Outgoing::sent_at`). The store writes that value
  as `sent_at`, instead of `now()`. A repeat the store recognises by its key keeps the time it
  was first stored with and is not delivered again, as before.
- The time is never earlier than the previous message's from the same owner: a wall clock that
  steps back does not reorder a room's times against its seqs. A new owner starts from its own
  clock.
- `rt::Message` carries the time, and so does the node channel's `Deliver`, as a 64-bit count of
  microseconds since the Unix epoch, between the key and the body. The node channel moves to
  **version 6**.
- Every `message` frame, live, resumed and from history, gains `at`: Unix milliseconds,
  floored.

## Consequences

- A release that carries this moves the node channel from version 5 to 6: roll it out with the
  chat Deployment's strategy set to `Recreate` (deploy/kubernetes/RUNBOOK.md, section 3).
- `at` is additive; a client that ignores it is unaffected. Each message frame is about
  20 bytes longer, and `message_wire_size`, which budgets resumes and history pages, counts it.
- `sent_at` follows the owning node's clock, not the database's. Nodes run NTP; a skew between
  owners shows as a step in a room's times when its owner changes, never as a reordering within
  one owner.
- Messages stored before this keep the `sent_at` the database gave them.
