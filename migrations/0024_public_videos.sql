-- ADR-0121: a video may be public, played by anyone who has its id, signed in or not. The check
-- is widened, never narrowed, so every row already holds it: NOT VALID, so that nothing scans
-- videos, as 0016 did. A stream's visibility stays as it was (live_streams_visibility).
ALTER TABLE videos
    DROP CONSTRAINT videos_visibility,
    ADD CONSTRAINT videos_visibility
        CHECK (visibility IN ('private', 'room', 'unlisted', 'public')) NOT VALID;
