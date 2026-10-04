-- The media generation of a room's call (ADR-0050, ADR-0095): the number its call's media room
-- on the SFU is named by, "<room>:<generation>". It moves forward only, by one, in the owner's
-- fenced write (owner_generation = the generation the owner holds), and only to put someone out
-- of the call or to end it: closing the old media room is what keeps an expelled device out,
-- since the SFU keeps refreshing a connected client's credential. A new owner reads it under its
-- own generation, so a deposed owner can neither move it nor read it. Rooms start at 1, the
-- generation every call ran in before this column (ADR-0087).
ALTER TABLE room_state ADD COLUMN media_generation bigint NOT NULL DEFAULT 1
    CHECK (media_generation >= 1);
