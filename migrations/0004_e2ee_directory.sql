-- MLS devices and their key packages (ADR-0038). The server stores what clients publish and
-- never reads it: key packages are opaque bytes.

-- A deregistered device keeps its row, retired: its id can never come back, so a stale client
-- cannot resurrect it, and anything still naming it resolves to "revoked", not "unknown".
CREATE TABLE devices (
    id            uuid PRIMARY KEY,
    user_id       text NOT NULL,
    registered_at timestamptz NOT NULL DEFAULT now(),
    revoked_at    timestamptz
);
CREATE INDEX devices_by_user ON devices (user_id);

-- Single use: a fetch deletes the row it hands out. 8192 is kMaxKeyPackageBytes.
CREATE TABLE key_packages (
    id           bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    device_id    uuid NOT NULL REFERENCES devices (id),
    body         bytea NOT NULL,
    published_at timestamptz NOT NULL DEFAULT now(),
    CONSTRAINT key_package_size CHECK (octet_length(body) BETWEEN 1 AND 8192)
);
-- Fetches take a device's oldest package first.
CREATE INDEX key_packages_by_device ON key_packages (device_id, id);

-- One row per epoch transition a room's group has made, holding the commit that made it: the
-- key lets the first commit built at an epoch be the only one accepted, and because the claim
-- and the commit are one row, an accepted epoch always has its commit for members to fetch.
-- 65536 is kMaxCommitBytes. No foreign key to the rooms: those belong to chat, and a commit
-- outliving its room is harmless.
CREATE TABLE mls_epochs (
    room_id     uuid NOT NULL,
    epoch       bigint NOT NULL,
    device_id   uuid NOT NULL REFERENCES devices (id),
    body        bytea NOT NULL,
    accepted_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (room_id, epoch),
    CONSTRAINT epoch_not_negative CHECK (epoch >= 0),
    CONSTRAINT commit_size CHECK (octet_length(body) BETWEEN 1 AND 65536)
);
