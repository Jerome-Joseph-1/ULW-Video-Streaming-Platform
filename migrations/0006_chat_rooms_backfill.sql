-- Records every room that existed before chat_rooms (0005) as a group chat: closed, as every
-- room was until then (ADR-0052). Otherwise a room with no kind recorded would take the kind
-- of its next join, and one with members listed but no kind recorded could be opened by the
-- server. From here on a join records only a closed kind, adding a member records the room
-- closed first, and only the server records a room live, never while it lists members.
-- Rooms come from room_state (every room the room plane created) and from chat_members (rooms
-- listed before they were ever joined). A kind already recorded stays.
INSERT INTO chat_rooms (room_id, kind)
SELECT room_id, 'group_chat' FROM room_state
UNION
SELECT room_id, 'group_chat' FROM chat_members
ON CONFLICT (room_id) DO NOTHING;
