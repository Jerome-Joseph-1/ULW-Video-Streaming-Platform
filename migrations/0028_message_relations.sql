-- ADR-0136: what a message is to an earlier one of its room (a reply, a message in its thread, an
-- edit or a reaction), kept apart from its body, which stays opaque. Written by the room's owner
-- in the same fenced statement as the message (room_store.cpp, kAppendMessage); a redaction
-- drops its row (ADR-0135), and a stream's live chat drops it with its message.
--
-- A table of its own, not columns on chat_messages: an index on chat_messages would be built
-- without CONCURRENTLY, which a migration's transaction does not allow, under a SHARE lock that
-- holds every send for as long as it takes to read the whole table. This one is new and empty,
-- so it and its indexes are made in moments, and nothing else is locked or scanned.
--
-- The primary key finds a message's relation, for its frames (history pages join it). The index
-- lists what relates to a message, of one type in seq order or of any, and counts it, without
-- reading chat_messages. No foreign key, as for chat_messages: the owner writes both together.
-- The table holds no body, so a violated check reports nothing a message says.
CREATE TABLE chat_message_relations (
    room_id  uuid NOT NULL,
    seq      bigint NOT NULL,
    rel_type text COLLATE "C" NOT NULL
        CONSTRAINT chat_message_relations_type CHECK (rel_type IN ('reply', 'thread', 'edit', 'react')),
    rel_seq  bigint NOT NULL,
    PRIMARY KEY (room_id, seq)
);
CREATE INDEX chat_message_relations_to
    ON chat_message_relations (room_id, rel_seq, rel_type, seq);
