-- What each ended live stream became (ADR-0055): its video, or why it could not be recorded.
-- A stream's end can be seen more than once, by a packager that restarts or is started again
-- for an ended stream, or by two at once; the stream id as the key makes every sighting after
-- the first write nothing. No foreign key: the row outlives a purged video, so a purge does
-- not let the stream be recorded a second time.
CREATE TABLE live_recordings (
    stream_id   text PRIMARY KEY,
    video_id    uuid,
    failure     text,
    recorded_at timestamptz NOT NULL DEFAULT now(),
    CONSTRAINT video_or_failure CHECK ((video_id IS NULL) <> (failure IS NULL)),
    -- A reason says something; an empty one would read back as neither a video nor a reason.
    CONSTRAINT failure_says_why CHECK (length(failure) > 0)
);
