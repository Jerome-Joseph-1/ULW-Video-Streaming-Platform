# 0135. A message can be redacted

Status: Accepted
Date: 2026-10-22
Amends: ADR-0054 ("History is never deleted": a message's body can now be blanked, in place)

## Context

Nothing removed or blanked a stored message (#211). A sender could not take back what they
sent, a group's admin could not remove abuse, and the backend could not honour a takedown. A
client's "delete" was a quiet message that every client had to apply itself, while history,
resumes and new devices kept serving the original bytes.

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| `redact` blanks the stored body in place and marks the row; every node is told with a room plane notice | No seq moves and nothing is deleted, so pages and gaps read as before; history, resumes and every node's kept copy all say `redacted` | The notice is best effort, as a ring's is; a node that misses it resumes the old body until the room's kept messages roll over | Accepted |
| Delete the row | The bytes are gone from the table | A hole in seq reads as a gap that history can never fill, and a resend of the key would be sequenced again | Rejected |
| A sequenced "redaction" message | Ordered, resumed and stored like any message | Every node still serves the original, and each client must apply it, which is the problem | Rejected |
| A delete marker sent quiet (ADR-0128), as now | Nothing to build | The server keeps and serves the original forever | Rejected |

## Decision

- The socket's `redact` takes `room` and `seq`, in a room the socket joined, charged as a send.
  The message's sender may redact it, and so may an admin of the room (a group's). The service
  API's `redact_message` takes the same, for any stored message.
- The store sets the body to empty and `chat_messages.redacted` (migration 0027, a constant
  default, so no row is rewritten) in one statement by primary key, with the actor checked in it.
  The seq, sender, key and time stay. Redacting again answers the same. A send repeated under a
  redacted message's key is answered with its seq, and restores nothing.
- Once the store has, the asking node hands a notice to the room's members on every node
  through the room's owner (ADR-0091): layout 5 of the room notices typing and read marks use
  (ADR-0138), the room and the seq. Each node blanks what it keeps of the
  message and sends every socket joined to the room `{"type":"redacted","room","seq"}`. The asker
  is answered the same frame, so it may see it twice.
- History, resumes and catch-ups send a redacted message with `"redacted":true` and an empty
  `body`.
- The node channel is unchanged: an older node counts the notice as malformed and drops it.

## Consequences

- Redaction is not erasure. The old row version stays in the table until vacuum, and in WAL
  and backups until they expire. A user's deletion request still needs retention to be settled.
- A node that misses the notice (a peer link that fails, an owner changing hands) resumes the
  old body until it drops the message from what it keeps of the room: its newest 256 KiB, held
  while the node has clients there and 30 s after. Redacting again tells every node again.
  History from the store is always blank.
- `messages_redacted_total`, `redact_refusals_total{reason="not_found"|"not_allowed"}` and
  `redacted_events_total` count it.
