-- ADR-0129: the call each room is ringing, written by the room's owner, so that what one owner
-- knew outlives it. The next owner tells the members a ring it cannot carry on has ended,
-- rather than leaving their devices ringing in silence, and a socket that connects hears the
-- rings that are its user's at once. One row per room: a room rings one call at a time. A row
-- past expires_at rings nobody, whatever is left of it; any write deletes those an hour old.
CREATE TABLE chat_call_rings (
    room_id    uuid PRIMARY KEY,
    call_id    uuid NOT NULL,
    caller     text COLLATE "C" NOT NULL,
    is_group   boolean NOT NULL,
    callees    text[] NOT NULL,
    started_at timestamptz NOT NULL DEFAULT now(),
    expires_at timestamptz NOT NULL
);
-- A socket's user asks for the rings that are theirs.
CREATE INDEX chat_call_rings_callees ON chat_call_rings USING gin (callees);
CREATE INDEX chat_call_rings_expires ON chat_call_rings (expires_at);
