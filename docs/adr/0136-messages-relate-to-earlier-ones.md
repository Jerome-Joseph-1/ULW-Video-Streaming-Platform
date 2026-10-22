# 0136. Messages relate to earlier ones

Status: Accepted
Date: 2026-10-22
Amends: ADR-0128 (the node channel moves to version 8)

## Context

Bodies are opaque (ADR-0054), so a reply, a thread, an edit or a reaction was something only
clients could see, by decrypting and reading every message. Nothing could list a thread's
messages, count a message's replies, or fetch the edits of one message without paging the whole
room (#215). Matrix answers this with `m.relates_to`, a relation the server can index while the
content stays encrypted.

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| An optional `rel` (type and seq) on `send`, carried with the message to its store and frames, kept in a table of its own with an index | The server lists and counts relations without reading a body; it travels the path `quiet` did; the new table is empty, so its migration locks and scans nothing | A relation on the node channel (version 8); a probe per message for history's frames | Accepted |
| Columns on `chat_messages` and an index there | One row per message, no join | The index is built inside the migration's transaction, without `CONCURRENTLY`, under a SHARE lock that holds every send while it reads the whole table | Rejected |
| Leave relations in bodies | Nothing for chat to do | No thread listing or reply counts, ever, for an encrypted room | Rejected |

## Decision

- `send` takes an optional `rel`: `{"type":"reply"|"thread"|"edit"|"react","seq":N}`, `N` an
  earlier message of the room. A seq past what the node has delivered is refused `bad_rel`.
- The relation travels as `quiet` does (ADR-0128): flags bit 1 on the node channel's Send and
  Deliver, then a type byte and the seq (version 8); the owner's fenced write stores it in
  `chat_message_relations` (migration 0028), in the same statement as the message, for a seq
  below the message's own only. A lossy room prunes it with its message.
- Every `message` frame carries it as `rel`, live, resumed and from history.
- `history` takes `rel_to` (and optionally `rel_type`): the messages relating to that seq, paged
  by their own seqs with `before` and `after`, ended with `history` naming `rel_to`.
- `rel_counts` (`room`, 1 to 100 `seqs`, optional `rel_type`, `reply` by default) answers, per
  seq, how many messages relate to it and the newest one's seq. The service API has it too.
- A redaction (ADR-0135) drops the message's relation: a redacted reply or reaction is counted
  no more.

## Consequences

- Threads, reply counts and edit histories need no full read of the room. A relation is
  metadata the server sees: who reacted or replied to which message, though not with what.
- The node channel moves from 7 to 8: roll the release out with `Recreate` (RUNBOOK section 3).
  It ships with version 7, so one `Recreate` covers both.
- History reads one more primary key per message; counting a message with very many relations
  reads that many index entries.
- `relation_refusals_total{reason="bad_rel"}` and `rel_counts_total` count it.
