-- A stream's live chat closes when the stream ends (ADR-0092): the stream service sets
-- closed_at in the transaction that ends the stream, and from then on a join of the room is
-- refused as a join of a room not open (`not_live`), and the room reads as closed to a call's
-- check (message_sql::kAdmits, kAccess). Only an open room (stream_live_chat) closes, and its
-- kind stays as it was recorded: closing is not a change of kind.
--
-- Both statements change the catalog alone, so the table's ACCESS EXCLUSIVE lock is held for
-- moments, not for a scan of every room. The constraint is added NOT VALID: it holds every row
-- written from now on, and every row already there has closed_at NULL, the column being new.
-- VALIDATE CONSTRAINT, which would only confirm that, is left out: the migrator runs each file
-- in one transaction, so it would scan the table under the lock ADD COLUMN took, the very wait
-- NOT VALID avoids; a later migration of its own may validate it under SHARE UPDATE EXCLUSIVE.
ALTER TABLE chat_rooms ADD COLUMN closed_at timestamptz;
ALTER TABLE chat_rooms ADD CONSTRAINT chat_rooms_only_live_closes
    CHECK (closed_at IS NULL OR kind = 'stream_live_chat') NOT VALID;
