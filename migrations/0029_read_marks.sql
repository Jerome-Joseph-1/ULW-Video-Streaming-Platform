-- ADR-0138: how far each member has read each room, for a client to show "Seen". A table of its
-- own rather than a column of chat_members: a mark moves as often as its member reads, and an
-- update of chat_members would write a new version of the row every join and listing reads.
-- Here the update changes no indexed column, so with room left on each page (fillfactor) it stays
-- a heap-only update, and nothing but the row is written. A mark only moves forward, and goes
-- with its member: removing someone from a room removes their mark.
CREATE TABLE chat_read_marks (
    room_id uuid NOT NULL,
    user_id text COLLATE "C" NOT NULL,
    seq     bigint NOT NULL CHECK (seq > 0),
    read_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (room_id, user_id),
    FOREIGN KEY (room_id, user_id) REFERENCES chat_members (room_id, user_id) ON DELETE CASCADE
) WITH (fillfactor = 80);
