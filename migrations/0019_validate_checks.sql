-- Validates the checks 0012 to 0016 added NOT VALID, each of which every row already holds by
-- construction: a new column's constant default (0012's closed_at, 0014's media_generation,
-- 0015's role, 0016's visibility and visibility_room), or a check that replaced a narrower one
-- (0013's end_reason). Until now PostgreSQL held new and updated rows to them but did not count
-- on them for the rows before; from here on every row is known to hold them.
--
-- VALIDATE CONSTRAINT takes a SHARE UPDATE EXCLUSIVE lock, under which reads and writes of the
-- table go on; only another schema change, or a manual VACUUM, waits. Each scans its table once.
-- The migrator runs this file in one transaction, so each table's lock is held until the commit,
-- after the last scan: chat rooms, live streams, room state, chat members and videos are read in
-- turn, while traffic goes on.
--
-- Left NOT VALID: chat_members_user_id and chat_rooms_named_kind (0015). Rows from before 0015
-- were written by operators' SQL as well as by the service, and a single row that breaks either
-- would make this migration fail and the gateway's init container retry it forever. The RUNBOOK
-- has the queries that find such rows; validate those two by hand once they return none.

ALTER TABLE chat_rooms VALIDATE CONSTRAINT chat_rooms_only_live_closes;
ALTER TABLE live_streams VALIDATE CONSTRAINT live_streams_end_reason;
ALTER TABLE room_state VALIDATE CONSTRAINT room_state_media_generation;
ALTER TABLE chat_members VALIDATE CONSTRAINT chat_members_role;
ALTER TABLE videos VALIDATE CONSTRAINT videos_visibility;
ALTER TABLE videos VALIDATE CONSTRAINT videos_visibility_room;
