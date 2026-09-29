-- One owning node per room (ADR-0015). A claim raises owner_generation, and every owner write
-- names the generation it holds, so a former owner's writes match nothing.
CREATE TABLE room_assignments (room_id uuid PRIMARY KEY, owner_node text NOT NULL,
  owner_generation bigint NOT NULL DEFAULT 1, heartbeat_at timestamptz NOT NULL DEFAULT now());
CREATE TABLE room_state (room_id uuid PRIMARY KEY REFERENCES room_assignments(room_id),
  owner_generation bigint NOT NULL, last_seq bigint NOT NULL DEFAULT 0, kind text NOT NULL,
  delivery text NOT NULL);   -- durable | lossy

-- Where each chat node takes connections from the others, and which run of it holds the name
-- (ADR-0033): a name is held by one incarnation at a time, from started_at, for as long as its
-- heartbeats keep seen_at fresh. A restarted pod comes back under a new name, so rows of
-- departed nodes stay behind; nothing reads them once no room names their node.
CREATE TABLE chat_nodes (
    node_id     text PRIMARY KEY,
    address     text NOT NULL,
    incarnation uuid NOT NULL,
    started_at  timestamptz NOT NULL DEFAULT now(),
    seen_at     timestamptz NOT NULL DEFAULT now()
);

-- Every node caches room owners and refreshes the cache from this channel. Firing on the
-- generation, not on every update, keeps heartbeats (which touch only heartbeat_at) silent.
CREATE FUNCTION notify_room_owner() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    PERFORM pg_notify('room_owner',
                      NEW.room_id::text || ' ' || NEW.owner_generation || ' ' || NEW.owner_node);
    RETURN NULL;
END
$$;
CREATE TRIGGER room_owner_changed AFTER INSERT OR UPDATE OF owner_generation ON room_assignments
    FOR EACH ROW EXECUTE FUNCTION notify_room_owner();
