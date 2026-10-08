-- Member lists that their users change (ADR-0096): a direct chat opened by one of its two users,
-- a group chat created with its first members, members added and removed by the group's admins,
-- and a member leaving. Until now only operators changed the lists, in SQL (RUNBOOK section 3);
-- they still may, and everything below holds for their statements as much as for the service's.
--
-- Applied after 0011 to 0014 (live streams, group calls): the migrator refuses a version older
-- than the newest applied, so this file must never reach a database before them.
--
-- Order matters: the migrator runs this file in one transaction, and every lock a statement takes
-- is held until that transaction commits. So the index is built first, under the SHARE lock
-- CREATE INDEX takes on chat_members: while it builds, joins and every other read of the table
-- proceed, and only writes to it (members added or removed, by a command or an operator) wait.
-- The statements after it change the catalog alone, scan nothing, and take ACCESS EXCLUSIVE
-- locks on chat_members and chat_rooms that are held only for the moments between them and the
-- commit. Had an ALTER TABLE come first, its ACCESS EXCLUSIVE lock would have been held for the
-- whole build, and every join would have waited for it.

-- A user's rooms, paged by room id (IMessageStore::rooms_of), and the count of rooms a user is
-- listed in that bounds what one user creates (kMaxRoomsPerUser). The primary key leads with the
-- room, so without this every listing would read the whole table. Built without CONCURRENTLY,
-- which a transaction does not allow (RUNBOOK).
CREATE INDEX chat_members_by_user ON chat_members (user_id, room_id);

-- Each member has a role. The creator of a group chat is its admin, and only admins add or
-- remove others; everyone else, the two of a direct chat and every row from before this
-- migration, is a member. A column with a constant default rewrites nothing. The checks are
-- added NOT VALID so that nothing scans chat_members: every existing row holds the default role,
-- and every new or updated row is checked. User ids are at most 128 bytes and hold no white
-- space, as the service parses them (core::UserId); an operator's INSERT is held to the same.
-- A later migration validates both (RUNBOOK).
ALTER TABLE chat_members ADD COLUMN role text NOT NULL DEFAULT 'member';
ALTER TABLE chat_members ADD CONSTRAINT chat_members_role
    CHECK (role IN ('member', 'admin')) NOT VALID;
ALTER TABLE chat_members ADD CONSTRAINT chat_members_user_id
    CHECK (length(user_id) <= 128 AND user_id !~ '\s') NOT VALID;

-- Rooms the service names from what they are for (ADR-0096), as it names a stream's chat
-- (0008): an RFC 9562 version 8 UUID whose first byte says what named it. 0x03 is the direct
-- chat of a pair of users, 0x04 a group chat named by its creator and their request. Such a room
-- is only ever recorded as that kind: a join of one asks for it whatever the client said, the
-- room plane creates an unrecorded one as it, and this refuses anything else, so that nobody
-- can take a pair's direct chat by recording its id first as a group chat. NOT VALID, as above:
-- no row before this migration has such an id unless a client joined one at random, and that
-- row admits nobody and is forgotten by the reaper (ADR-0075) all the same.
ALTER TABLE chat_rooms ADD CONSTRAINT chat_rooms_named_kind
    CHECK (get_byte(uuid_send(room_id), 6) >> 4 <> 8
           OR get_byte(uuid_send(room_id), 0) NOT IN (3, 4)
           OR (get_byte(uuid_send(room_id), 0) = 3 AND kind = 'direct_chat')
           OR (get_byte(uuid_send(room_id), 0) = 4 AND kind = 'group_chat')) NOT VALID;

-- Tells every chat node when a member list changes, however it changed: "+ <room> <user>" for
-- a member listed, "- <room> <user>" for one taken off, and "* <room> <role> <user>" for a
-- member whose role changed (a group's last admin leaving promotes another; an operator may
-- grant admin). An UPDATE that moves a row to another room or user is a removal and an
-- addition. A node tells the user's sockets, and the sockets in the room, and takes a removed
-- user's sockets out of the room (ADR-0073). Nodes from before this migration listen on
-- chat_member_removed (0009), which still fires; nodes from here on listen on this channel only.
-- Drop the 0009 triggers once no node older than this migration runs (RUNBOOK). Created last:
-- a trigger takes the table's SHARE ROW EXCLUSIVE lock, and the ALTERs above hold more already.
CREATE FUNCTION notify_chat_members() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    IF TG_OP = 'UPDATE' AND OLD.room_id = NEW.room_id AND OLD.user_id = NEW.user_id THEN
        PERFORM pg_notify('chat_members',
                          '* ' || NEW.room_id::text || ' ' || NEW.role || ' ' || NEW.user_id);
        RETURN NULL;
    END IF;
    IF TG_OP IN ('DELETE', 'UPDATE') THEN
        PERFORM pg_notify('chat_members', '- ' || OLD.room_id::text || ' ' || OLD.user_id);
    END IF;
    IF TG_OP IN ('INSERT', 'UPDATE') THEN
        PERFORM pg_notify('chat_members', '+ ' || NEW.room_id::text || ' ' || NEW.user_id);
    END IF;
    RETURN NULL;
END
$$;
CREATE TRIGGER chat_members_changed AFTER INSERT OR DELETE ON chat_members
    FOR EACH ROW EXECUTE FUNCTION notify_chat_members();
CREATE TRIGGER chat_members_moved AFTER UPDATE OF room_id, user_id ON chat_members
    FOR EACH ROW
    WHEN (OLD.room_id IS DISTINCT FROM NEW.room_id OR OLD.user_id IS DISTINCT FROM NEW.user_id)
    EXECUTE FUNCTION notify_chat_members();
CREATE TRIGGER chat_members_role_changed AFTER UPDATE OF role ON chat_members
    FOR EACH ROW
    WHEN (OLD.role IS DISTINCT FROM NEW.role AND OLD.room_id = NEW.room_id
          AND OLD.user_id = NEW.user_id)
    EXECUTE FUNCTION notify_chat_members();
