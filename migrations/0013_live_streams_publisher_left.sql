-- A stream can end because LiveKit said its publisher left and it did not come back within the
-- gateway's grace for reconnects (ADR-0093): end_reason 'publisher_left'. The wider constraint is
-- added NOT VALID, as 0012's is: every row already holds it (the one it replaces was narrower),
-- so nothing is gained by a scan under the ACCESS EXCLUSIVE lock, and new rows are checked all
-- the same.
ALTER TABLE live_streams DROP CONSTRAINT live_streams_end_reason;
ALTER TABLE live_streams ADD CONSTRAINT live_streams_end_reason
    CHECK (end_reason IN ('owner', 'finished', 'failed', 'timeout', 'publisher_left')) NOT VALID;
