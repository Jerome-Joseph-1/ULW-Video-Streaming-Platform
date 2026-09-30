-- When each room's kind was recorded. A join of a room nobody has recorded records it, and is
-- refused, since such a room lists nobody (ADR-0054): rows that nothing ever uses. The upload
-- reaper forgets a room recorded more than a day ago that lists no members and was never
-- resolved on the room plane, so never held a message; the chat service bounds how many a user's
-- joins record before then. Rows already here count as recorded now. The index is what the
-- reaper walks, oldest first, within the last week.
ALTER TABLE chat_rooms ADD COLUMN recorded_at timestamptz NOT NULL DEFAULT now();
CREATE INDEX chat_rooms_recorded_at ON chat_rooms (recorded_at);
