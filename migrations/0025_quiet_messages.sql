-- ADR-0128: a message its sender asked to notify nobody (a reaction, an edit). A column with a
-- constant default: Postgres records the default and rewrites no row, so this takes moments
-- under the ALTER's lock however many messages there are.
ALTER TABLE chat_messages ADD COLUMN quiet boolean NOT NULL DEFAULT false;
