-- Every message a room's owner sequenced (ADR-0015), by (room, seq). The body is opaque bytes,
-- plaintext or MLS ciphertext alike: bytea, never text, so that nothing can collate, search or
-- index what it says. The table has no check constraint: a violated one reports the failing
-- row, body included, in the error and the server log. The store bounds the body's size
-- before it sends one.
--
-- History pages walk the primary key in either direction; a backward scan of (room_id, seq) is
-- the (room_id, seq DESC) order, so no second index is needed. No foreign key to room_state:
-- a seq exists only because the room's owner took it from room_state, and rooms are never
-- deleted, so the check could not fail and would cost a lookup per message.
CREATE TABLE chat_messages (
    room_id uuid NOT NULL,
    seq     bigint NOT NULL,
    sender  text NOT NULL,
    body    bytea NOT NULL,
    sent_at timestamptz NOT NULL,
    PRIMARY KEY (room_id, seq)
);

-- Who belongs to a room. Ids compare bytewise ("C"), so that pages by id follow the order the
-- application compares them in, whatever the database's default collation.
CREATE TABLE chat_members (
    room_id uuid NOT NULL,
    user_id text COLLATE "C" NOT NULL,
    PRIMARY KEY (room_id, user_id)
);
