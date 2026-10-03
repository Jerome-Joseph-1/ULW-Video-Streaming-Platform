-- The live streams the stream service started (ADR-0092). A row is a stream from the moment its
-- owner asks for one until it ends; the id is the stream's name everywhere else too (its
-- packager, its playlist under live/<id>/, its chat through live_chat_room, its recording in
-- live_recordings, keyed by the id's text).
--
-- state only moves forward: starting (the owner holds publisher tickets), live (the packager
-- runs and the media server relays the publisher to it), ended. The passphrase is what the
-- packager's SRT listener admits the relay with (ADR-0046): a secret, never sent to a client.
CREATE TABLE live_streams (
    id             uuid PRIMARY KEY,
    owner_id       text NOT NULL,
    state          text NOT NULL DEFAULT 'starting',
    srt_passphrase text NOT NULL,
    created_at     timestamptz NOT NULL,
    live_at        timestamptz,
    ended_at       timestamptz,
    end_reason     text,
    CONSTRAINT live_streams_state CHECK (state IN ('starting', 'live', 'ended')),
    CONSTRAINT live_streams_passphrase CHECK (char_length(srt_passphrase) BETWEEN 10 AND 79),
    CONSTRAINT live_streams_end_reason
        CHECK (end_reason IN ('owner', 'finished', 'failed', 'timeout')),
    -- Ended exactly when it has an end time and a reason.
    CONSTRAINT live_streams_ended CHECK (
        (state = 'ended') = (ended_at IS NOT NULL) AND (ended_at IS NULL) = (end_reason IS NULL)),
    -- Live is reached only through live_at, and a stream that went live keeps it once ended.
    CONSTRAINT live_streams_live CHECK (state <> 'live' OR live_at IS NOT NULL)
);

-- One unfinished stream per user: a second request for a stream answers the one already
-- running, and a stream must end before its owner starts another (live.md).
CREATE UNIQUE INDEX live_streams_one_unfinished ON live_streams (owner_id) WHERE state <> 'ended';
-- What the service's sweep and its capacity check read.
CREATE INDEX live_streams_unfinished ON live_streams (created_at) WHERE state <> 'ended';
