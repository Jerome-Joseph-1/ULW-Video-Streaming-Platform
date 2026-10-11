-- Who may watch a live stream (ADR-0105): unlisted (anyone signed in who has its id, as every
-- stream was until now), private (its owner alone) or the members of one chat room, which must
-- list the owner too. The owner sets it; the stream's recording keeps its own visibility as a
-- video (ADR-0097).
--
-- Applied after 0019: the migrator refuses a version older than the newest applied.
--
-- One ALTER, catalog changes only: a column with a constant default and one without rewrite
-- nothing. The checks are added and validated here, unlike 0016's on videos: validating scans
-- live_streams under the ALTER's ACCESS EXCLUSIVE lock, and the table holds one row per stream
-- ever started, a few an hour at the platform's caps (LiveSettings), so the scan takes
-- moments; only the stream service reads it, and its statements wait those moments.
ALTER TABLE live_streams
    ADD COLUMN visibility text NOT NULL DEFAULT 'unlisted',
    ADD COLUMN visibility_room uuid,
    ADD CONSTRAINT live_streams_visibility
        CHECK (visibility IN ('private', 'room', 'unlisted')),
    ADD CONSTRAINT live_streams_visibility_room
        CHECK ((visibility = 'room') = (visibility_room IS NOT NULL));
