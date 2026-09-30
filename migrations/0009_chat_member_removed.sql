-- Tells every chat node when someone leaves a room's member list, however the row went: the
-- product or an operator deletes it in the database, as nothing in chat_server does. A node
-- stops delivering the room to that user's connections at once, instead of when they close
-- (ADR-0073). The payload is "<room> <user>"; a room id has no space, so everything after the
-- first is the user id. An update that changes a row's room or user removes the old pair; one
-- that writes them unchanged removes nobody, and must not take a listed member out of the room.
CREATE FUNCTION notify_chat_member_removed() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    PERFORM pg_notify('chat_member_removed', OLD.room_id::text || ' ' || OLD.user_id);
    RETURN NULL;
END
$$;
CREATE TRIGGER chat_member_removed AFTER DELETE ON chat_members
    FOR EACH ROW EXECUTE FUNCTION notify_chat_member_removed();
CREATE TRIGGER chat_member_moved AFTER UPDATE OF room_id, user_id ON chat_members
    FOR EACH ROW
    WHEN (OLD.room_id IS DISTINCT FROM NEW.room_id OR OLD.user_id IS DISTINCT FROM NEW.user_id)
    EXECUTE FUNCTION notify_chat_member_removed();
