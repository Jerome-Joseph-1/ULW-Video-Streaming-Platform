-- Append-only analytics. No foreign key: a batch of views must not fail as a whole because
-- one of its videos was purged after it was watched.
CREATE TABLE view_events (
    video_id  uuid NOT NULL,
    viewer_id text NOT NULL,
    viewed_at timestamptz NOT NULL
);
CREATE INDEX view_events_by_video ON view_events (video_id, viewed_at);
