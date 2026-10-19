-- ADR-0120: a video that became ready or failed, for the notification hook. A trigger on videos
-- writes one row per such change; a chat node with ULW_NOTIFY_URL set takes the rows, oldest
-- first, and sends them as events. A row nobody takes is deleted an hour on, by the trigger
-- itself, so a deployment without the hook keeps at most an hour of them.
CREATE TABLE video_events (
    id           bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    video_id     uuid NOT NULL,
    owner_id     text NOT NULL,
    state        video_state NOT NULL,
    version      integer NOT NULL,
    duration_ms  integer,
    error_reason text,
    at           timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX video_events_at ON video_events (at);

-- Created last: a trigger takes the table's SHARE ROW EXCLUSIVE lock.
CREATE FUNCTION record_video_event() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    DELETE FROM video_events WHERE at < now() - interval '1 hour';
    INSERT INTO video_events (video_id, owner_id, state, version, duration_ms, error_reason)
    VALUES (NEW.id, NEW.owner_id, NEW.state, NEW.version, NEW.duration_ms, NEW.error_reason);
    RETURN NULL;
END
$$;
CREATE TRIGGER videos_settled AFTER UPDATE OF state ON videos
    FOR EACH ROW
    WHEN (NEW.state IN ('ready', 'failed') AND OLD.state IS DISTINCT FROM NEW.state
          AND NEW.deleted_at IS NULL)
    EXECUTE FUNCTION record_video_event();
