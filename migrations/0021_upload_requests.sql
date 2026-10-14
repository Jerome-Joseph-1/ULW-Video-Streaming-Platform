-- ADR-0108: a create's Idempotency-Key names the upload it made, per owner, for as long as the
-- upload exists. A new table, so nothing existing is scanned; its foreign key takes a SHARE ROW
-- EXCLUSIVE lock on uploads until the migration commits (writes wait, reads go on), and checks
-- nothing, as the table is empty. A row goes with its upload when the reaper purges the video.
CREATE TABLE upload_requests (
    owner_id        text NOT NULL,
    idempotency_key text NOT NULL,
    upload_id       uuid NOT NULL REFERENCES uploads (id) ON DELETE CASCADE,
    -- What the request asked for: a retry under the same key must ask for the same.
    fingerprint     text NOT NULL,
    created_at      timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (owner_id, idempotency_key),
    CONSTRAINT upload_request_key_shape CHECK (idempotency_key ~ '^[!-~]{1,255}$')
);
-- The purge deletes uploads by id: the cascade finds their keys through this.
CREATE INDEX upload_requests_upload ON upload_requests (upload_id);
