-- ADR-0111: who has a socket open where, for the notification hook. Written only by chat nodes
-- that have ULW_NOTIFY_URL set; empty otherwise. New tables, so nothing existing is scanned or
-- locked.
--
-- A run of a chat node, by a random tag it draws at start, and when it last renewed. A node whose
-- renewal is older than the lease counts for nothing; one older than ten minutes is deleted, its
-- sockets with it, by the next renewal of any node.
CREATE TABLE notify_nodes (
    node_tag   bigint PRIMARY KEY,
    renewed_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX notify_nodes_renewed ON notify_nodes (renewed_at);

-- A device with a socket at a node: the device the socket named when it connected, or the nil
-- UUID for one that named none (it counts as its user's socket, not as a device).
CREATE TABLE notify_sockets (
    node_tag  bigint NOT NULL REFERENCES notify_nodes (node_tag) ON DELETE CASCADE,
    user_id   text COLLATE "C" NOT NULL,
    device_id uuid NOT NULL,
    PRIMARY KEY (node_tag, user_id, device_id)
);
-- Who is connected is asked by user.
CREATE INDEX notify_sockets_user ON notify_sockets (user_id);

-- When a member was last told of a room's messages: a message notifies them again only once
-- the interval has passed, or after they connected (which deletes their marks).
CREATE TABLE notify_marks (
    room_id uuid NOT NULL,
    user_id text COLLATE "C" NOT NULL,
    told_at timestamptz NOT NULL,
    PRIMARY KEY (room_id, user_id)
);
CREATE INDEX notify_marks_user ON notify_marks (user_id);
CREATE INDEX notify_marks_told ON notify_marks (told_at);
