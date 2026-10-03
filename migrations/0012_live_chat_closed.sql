-- A stream's live chat closes when the stream ends (ADR-0092): the stream service sets
-- closed_at in the transaction that ends the stream, and from then on a join of the room is
-- refused as a join of a room not open (`not_live`), and the room reads as closed to a call's
-- check (message_sql::kAdmits, kAccess). Only an open room (stream_live_chat) closes, and its
-- kind stays as it was recorded: closing is not a change of kind.
ALTER TABLE chat_rooms ADD COLUMN closed_at timestamptz;
ALTER TABLE chat_rooms ADD CONSTRAINT chat_rooms_only_live_closes
    CHECK (closed_at IS NULL OR kind = 'stream_live_chat');
