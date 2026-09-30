# 0075. A chat room's record that nothing used is forgotten

Status: Accepted
Date: 2026-09-30

## Context

ADR-0054 records each room's kind in `chat_rooms` the first time anything names the room, and
says the kind "is recorded once, and never changes". A join of a room nobody has recorded
records the kind it names and is refused, since such a room lists nobody. So any client could
add rows without end by joining made-up room ids, and nothing ever removed one: the security
review of 2026-09-30 raised it. The chat service now bounds how many rooms a user's refused
joins record per node (20 at once, then one a minute); what they did record still has to go.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Record nothing on a refused join | No row to remove | Rejected: the record is what keeps a room closed that is listed afterwards and opened in between (ADR-0054's ordering with `record_live`) |
| The upload reaper deletes, a day on, a direct or group chat with no members that the room plane never resolved and that holds no message | The reaper is already a periodic Job with a Postgres session; the row is exactly the record a refused join leaves | Accepted |
| Only rooms recorded in the last week | One index range per pass | Rejected: a room that falls out of the window stays forever |
| Walk every room recorded before the cutoff in each pass | Nothing is missed | Rejected: a pass would cost every room there ever was, most of them in use |

## Decision

- Migration 0010 adds `chat_rooms.recorded_at` (rows already there count as recorded when it
  runs), an index on `(recorded_at, room_id)`, and `chat_rooms_forget_cursor`, one row.
- Each reaper pass walks `chat_rooms` in `(recorded_at, room_id)` order from the cursor, among
  rooms recorded more than a day ago, 100 rooms per statement and at most 10,000 per pass, and
  deletes those that are a direct or group chat, list no members, have no `room_assignments` row
  and no `chat_messages` row. Rows another statement holds are skipped. The statement moves the
  cursor past what it walked; one that walks fewer than its 100 has reached the cutoff and puts
  the cursor back to the start. So every room is looked at again once a lap ends, however old,
  and no pass costs more than its 10,000.
- A stream's live chat is never deleted: the server opens it, it lists nobody by design, and it
  is not what a join records.
- This amends ADR-0054: a room's kind never changes while anything uses the room, but a record
  nothing used may go, and a later join records the room again, as the kind that join names,
  which may be the other closed kind. Nothing had been let into the room, so no one's access
  changes.

## Consequences

- A lap takes `rooms / 10,000` passes, at one pass each 15 minutes: a million rooms are all
  looked at within a day.
- Migrations run in a transaction, so the index is built without `CONCURRENTLY`: while it is
  built, every join of a closed room, member listing and live chat opening waits. The release
  that carries 0010 is deployed off-peak (RUNBOOK).
- A member added between the reaper's check and its delete is left in a room with no kind
  recorded, which admits only members and is recorded closed by the next join.
