-- The demo's users and rooms. The chat service admits a direct or group chat's members only, and
-- no client command changes a member list yet (docs/integration/chat.md, "Member lists"), so
-- the demo lists them here. web/rooms.json names the same rooms for the page. Idempotent.
BEGIN;
INSERT INTO chat_rooms (room_id, kind) VALUES
    ('10000000-0000-4000-8000-000000000001', 'direct_chat'),  -- alice and bob
    ('10000000-0000-4000-8000-000000000002', 'direct_chat'),  -- alice and carol
    ('10000000-0000-4000-8000-000000000003', 'direct_chat'),  -- bob and carol
    ('20000000-0000-4000-8000-000000000001', 'group_chat'),   -- team: everyone
    ('30000000-0000-4000-8000-000000000001', 'group_chat')    -- encrypted: alice and bob
ON CONFLICT (room_id) DO NOTHING;
INSERT INTO chat_members (room_id, user_id) VALUES
    ('10000000-0000-4000-8000-000000000001', 'alice'),
    ('10000000-0000-4000-8000-000000000001', 'bob'),
    ('10000000-0000-4000-8000-000000000002', 'alice'),
    ('10000000-0000-4000-8000-000000000002', 'carol'),
    ('10000000-0000-4000-8000-000000000003', 'bob'),
    ('10000000-0000-4000-8000-000000000003', 'carol'),
    ('20000000-0000-4000-8000-000000000001', 'alice'),
    ('20000000-0000-4000-8000-000000000001', 'bob'),
    ('20000000-0000-4000-8000-000000000001', 'carol'),
    ('30000000-0000-4000-8000-000000000001', 'alice'),
    ('30000000-0000-4000-8000-000000000001', 'bob')
ON CONFLICT DO NOTHING;
COMMIT;
