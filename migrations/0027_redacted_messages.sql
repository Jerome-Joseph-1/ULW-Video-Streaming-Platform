-- ADR-0135: a message its sender, a group's admin or the service redacted. Its body is emptied in
-- place and this marks it, so that history says so; its seq, sender and key stay. A column with a
-- constant default: Postgres records the default and rewrites no row, so this takes moments
-- under the ALTER's lock however many messages there are, as 0025's did.
ALTER TABLE chat_messages ADD COLUMN redacted boolean NOT NULL DEFAULT false;
