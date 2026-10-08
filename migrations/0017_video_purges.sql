-- Videos deleted by their owner or taken down by the operator's backend (ADR-0100), waiting for
-- the reaper to remove their stored objects (the source and the HLS renditions, everything under
-- videos/<id>/) and then their row. The gateway's delete sets videos.deleted_at, which every read
-- already filters on, and inserts here in the same statement; the reaper reads this table, not
-- videos, so that finding what to purge never scans videos.
--
-- No foreign key: the reaper deletes the video's row and this one in one statement, and a key
-- would take videos' SHARE ROW EXCLUSIVE lock here for nothing. A new, empty table: nothing else
-- is locked or scanned, so this applies at once.
CREATE TABLE video_purges (
    video_id   uuid PRIMARY KEY,
    deleted_at timestamptz NOT NULL DEFAULT now()
);

-- The reaper takes the oldest first.
CREATE INDEX video_purges_by_age ON video_purges (deleted_at);
