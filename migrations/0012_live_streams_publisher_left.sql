-- A stream can end because LiveKit said its publisher left and it did not come back within the
-- gateway's grace for reconnects (ADR-0093): end_reason 'publisher_left'. Checking the
-- constraint again reads the table under an ACCESS EXCLUSIVE lock, which holds a few rows per
-- stream ever started; nothing else changes.
ALTER TABLE live_streams DROP CONSTRAINT live_streams_end_reason;
ALTER TABLE live_streams ADD CONSTRAINT live_streams_end_reason
    CHECK (end_reason IN ('owner', 'finished', 'failed', 'timeout', 'publisher_left'));
