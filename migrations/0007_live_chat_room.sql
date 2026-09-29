-- The room of a stream's live chat, as chat_server derives it from the stream's name
-- (apps/chat/src/live_chat.cpp, ADR-0057): the first 16 bytes of SHA-256 over 'ulw-live-chat:'
-- and the name, as an RFC 9562 version 8 UUID. Whatever opens a stream's chat records this room
-- live (ADR-0052's record_live, or the runbook's statement), and viewers joining the stream by
-- name reach it. A name the live packager would refuse is refused here too.
CREATE FUNCTION live_chat_room(stream text) RETURNS uuid
LANGUAGE plpgsql IMMUTABLE STRICT AS $$
DECLARE
    digest bytea;
BEGIN
    IF stream !~ '^[A-Za-z0-9_-]{1,64}$' THEN
        RAISE EXCEPTION 'not a stream name';
    END IF;
    digest := substring(sha256(convert_to('ulw-live-chat:' || stream, 'UTF8')) FROM 1 FOR 16);
    digest := set_byte(digest, 6, (get_byte(digest, 6) & 15) | 128);
    digest := set_byte(digest, 8, (get_byte(digest, 8) & 63) | 128);
    RETURN encode(digest, 'hex')::uuid;
END
$$;
