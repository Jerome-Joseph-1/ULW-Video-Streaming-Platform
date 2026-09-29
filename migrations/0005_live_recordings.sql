-- The video each ended live stream became (ADR-0054). A stream's end can be seen more than
-- once, by a packager that restarts or is started again for an ended stream; the stream id as
-- the key makes every sighting after the first write nothing. No foreign key: the row outlives
-- a purged video, so a purge does not let the stream be recorded a second time.
CREATE TABLE live_recordings (
    stream_id   text PRIMARY KEY,
    video_id    uuid NOT NULL,
    recorded_at timestamptz NOT NULL DEFAULT now()
);
