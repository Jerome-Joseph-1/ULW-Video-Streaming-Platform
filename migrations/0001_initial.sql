CREATE TYPE video_state AS ENUM ('init', 'uploading', 'processing', 'ready', 'failed');

CREATE TABLE videos (
    id           uuid PRIMARY KEY,
    owner_id     text NOT NULL,
    title        text NOT NULL,
    state        video_state NOT NULL DEFAULT 'init',
    error_reason text,
    duration_ms  integer,
    version      integer NOT NULL DEFAULT 0,
    created_at   timestamptz NOT NULL DEFAULT now(),
    updated_at   timestamptz NOT NULL DEFAULT now(),
    deleted_at   timestamptz,
    CONSTRAINT failed_has_reason CHECK (state <> 'failed' OR error_reason IS NOT NULL),
    CONSTRAINT ready_has_duration CHECK (state <> 'ready' OR duration_ms IS NOT NULL)
);
CREATE INDEX videos_by_owner ON videos (owner_id, created_at DESC);

CREATE TABLE uploads (
    id             uuid PRIMARY KEY,
    video_id       uuid NOT NULL REFERENCES videos (id) ON DELETE CASCADE,
    owner_id       text NOT NULL,
    -- A cache of what the object store reported; the store is authoritative.
    durable_offset bigint NOT NULL DEFAULT 0,
    -- Opaque ingest state of the object store adapter, handed back to it verbatim.
    backend_ref    text NOT NULL,
    object_key     text NOT NULL,
    chunk_size     integer NOT NULL,
    size_bytes     bigint NOT NULL,
    state          text NOT NULL DEFAULT 'active',
    created_at     timestamptz NOT NULL DEFAULT now(),
    expires_at     timestamptz NOT NULL,
    CONSTRAINT upload_state_known CHECK (state IN ('active', 'completed', 'aborted')),
    CONSTRAINT offset_within_size CHECK (durable_offset BETWEEN 0 AND size_bytes),
    CONSTRAINT completed_is_whole CHECK (state <> 'completed' OR durable_offset = size_bytes)
);
CREATE INDEX uploads_expiring ON uploads (state, expires_at) WHERE state = 'active';

CREATE TABLE jobs (
    id            bigserial PRIMARY KEY,
    video_id      uuid NOT NULL REFERENCES videos (id) ON DELETE CASCADE,
    kind          text NOT NULL,
    state         text NOT NULL DEFAULT 'queued',
    attempts      integer NOT NULL DEFAULT 0,
    max_attempts  integer NOT NULL DEFAULT 3,
    -- Raised by every claim; a holder's writes name the fence they were given.
    fence         bigint NOT NULL DEFAULT 0,
    locked_by     text,
    lease_expires timestamptz,
    run_after     timestamptz NOT NULL DEFAULT now(),
    last_error    text,
    progress_pct  smallint,
    request_id    text,
    source_key    text,
    created_at    timestamptz NOT NULL DEFAULT now(),
    CONSTRAINT job_state_known CHECK (state IN ('queued', 'running', 'done', 'failed')),
    CONSTRAINT running_has_lease
        CHECK (state <> 'running' OR (locked_by IS NOT NULL AND lease_expires IS NOT NULL)),
    CONSTRAINT progress_is_percent CHECK (progress_pct BETWEEN 0 AND 100)
);
-- At most one live job of a kind per video: a repeated enqueue is a no-op, not a second job.
CREATE UNIQUE INDEX one_live_job ON jobs (video_id, kind) WHERE state IN ('queued', 'running');
CREATE INDEX jobs_claimable ON jobs (run_after) WHERE state = 'queued';

CREATE TABLE renditions (
    video_id     uuid REFERENCES videos (id) ON DELETE CASCADE,
    height       integer NOT NULL,
    bitrate_bps  integer NOT NULL,
    playlist_key text NOT NULL,
    PRIMARY KEY (video_id, height)
);
