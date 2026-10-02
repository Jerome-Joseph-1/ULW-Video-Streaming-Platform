-- When each room's kind was recorded. A join of a room nobody has recorded records it, and is
-- refused, since such a room lists nobody (ADR-0054): rows that nothing ever uses. The upload
-- reaper forgets a direct or group chat recorded more than a day ago that lists no members and
-- was never resolved on the room plane, so never held a message; the chat service bounds how
-- many a user's joins record before then. Rows already here count as recorded now.
--
-- The reaper walks the rooms in (recorded_at, room_id) order, a bounded batch per statement,
-- from where its last statement stopped: chat_rooms_forget_cursor, one row. Once the walk
-- reaches the cutoff it starts over from the oldest, so every room is looked at again, however
-- old, and no statement costs more than its batch.
--
-- Migrations run in a transaction, so the index cannot be built CONCURRENTLY: the build holds a
-- SHARE lock on chat_rooms until the migration commits, and every statement that may write the
-- table waits for it, which is every join of a closed room (kAdmits), every member listing and
-- every live chat opened. Deploy it off-peak (deploy/askedin/RUNBOOK.md).
ALTER TABLE chat_rooms ADD COLUMN recorded_at timestamptz NOT NULL DEFAULT now();
CREATE INDEX chat_rooms_recorded_at ON chat_rooms (recorded_at, room_id);
CREATE TABLE chat_rooms_forget_cursor (
    one         boolean PRIMARY KEY DEFAULT true CHECK (one),
    recorded_at timestamptz NOT NULL DEFAULT '-infinity',
    room_id     uuid NOT NULL DEFAULT '00000000-0000-0000-0000-000000000000'
);
INSERT INTO chat_rooms_forget_cursor DEFAULT VALUES;
