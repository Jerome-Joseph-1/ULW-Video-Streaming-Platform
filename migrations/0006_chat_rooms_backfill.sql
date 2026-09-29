-- Records every room that existed before chat_rooms (0005) as a group chat, so that these rooms
-- become closed (ADR-0054). Until then no join was checked and every room admitted anyone; from
-- here on only their listed members are admitted. Otherwise a room with no kind recorded would
-- take the kind of its next join, and one with members listed but no kind recorded could be
-- opened by the server. A stream's room from before this migration cannot be opened: it needs a
-- new room id (RUNBOOK section 3). From here on a join records only a closed kind, adding a
-- member records the room closed first, and only the server records a room live, and only one
-- that lists no members.
-- Rooms come from room_state (every room the room plane created) and from chat_members (rooms
-- listed before they were ever joined). A kind already recorded stays.
INSERT INTO chat_rooms (room_id, kind)
SELECT room_id, 'group_chat' FROM room_state
UNION
SELECT room_id, 'group_chat' FROM chat_members
ON CONFLICT (room_id) DO NOTHING;
