-- The media generation of a room's call (ADR-0050, ADR-0095): the number its call's media room
-- on the SFU is named by, "<room>:<generation>". It moves forward only, by one, in the owner's
-- fenced write (owner_generation = the generation the owner holds), and only to put someone out
-- of the call or to end it: closing the old media room is what keeps an expelled device out,
-- since the SFU keeps refreshing a connected client's credential. A new owner reads it under its
-- own generation, so a deposed owner can neither move it nor read it. Rooms start at 1, the
-- generation every call ran in before this column (ADR-0087).
--
-- The column is added with a constant default, which rewrites nothing; the check is added NOT
-- VALID, so that the migration does not scan room_state under its ACCESS EXCLUSIVE lock: every
-- existing row holds the default, and every new or updated row is checked.
ALTER TABLE room_state ADD COLUMN media_generation bigint NOT NULL DEFAULT 1;
ALTER TABLE room_state ADD CONSTRAINT room_state_media_generation
    CHECK (media_generation >= 1) NOT VALID;

-- Who was put out of a room's call, per media generation: written in the same fenced statement
-- that moves the generation on (carried over from the previous generation, with the one put out
-- added), and read with it, so that a new owner refuses them as the old one did. A call ended for
-- everyone moves on with an empty list. Rows of generations before the previous one are deleted
-- by the next move.
CREATE TABLE room_media_expelled (
    room_id          uuid   NOT NULL REFERENCES room_state (room_id) ON DELETE CASCADE,
    media_generation bigint NOT NULL,
    user_id          text   NOT NULL,
    PRIMARY KEY (room_id, media_generation, user_id)
);
