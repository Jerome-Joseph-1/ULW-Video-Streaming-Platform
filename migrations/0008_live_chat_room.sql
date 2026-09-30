-- The room of a stream's live chat, as chat_server derives it from the stream's name
-- (apps/chat/src/live_chat.cpp, ADR-0070): an RFC 9562 version 8 UUID whose first byte is 0x01,
-- the tag of a stream's chat among rooms named by something else, and whose other bytes are the
-- first 15 of SHA-256 over 'ulw-live-chat:' and the name. Whatever opens a stream's chat records
-- this room live (ADR-0054's record_live, or the runbook's statement), and viewers joining the
-- stream by name reach it. A name the live packager would refuse is refused here too.
CREATE FUNCTION live_chat_room(stream text) RETURNS uuid
LANGUAGE plpgsql IMMUTABLE STRICT AS $$
DECLARE
    id bytea;
BEGIN
    IF stream !~ '^[A-Za-z0-9_-]{1,64}$' THEN
        RAISE EXCEPTION 'not a stream name';
    END IF;
    id := '\x01'::bytea
          || substring(sha256(convert_to('ulw-live-chat:' || stream, 'UTF8')) FROM 1 FOR 15);
    id := set_byte(id, 6, (get_byte(id, 6) & 15) | 128);
    id := set_byte(id, 8, (get_byte(id, 8) & 63) | 128);
    RETURN encode(id, 'hex')::uuid;
END
$$;

-- A room recorded live is a stream's: version 8 and tagged 0x01, as live_chat_room makes them.
-- The id is then what tells every node that a room gets the live chat's bounds (ADR-0070), with
-- no recorded kind to look up, and an operator's statement cannot open any other room, a
-- presence room (tagged 0x02) included.
ALTER TABLE chat_rooms ADD CONSTRAINT chat_rooms_live_is_a_stream
    CHECK (kind <> 'stream_live_chat'
           OR (get_byte(uuid_send(room_id), 6) >> 4 = 8 AND get_byte(uuid_send(room_id), 0) = 1));
