-- Each device's last-resort KeyPackage (RFC 9420 section 16.8, ADR-0102): handed out, and kept,
-- when the device's single-use packages (key_packages, migration 0004) have run out, so that the
-- device can still be invited while it is offline. One per device; publishing another replaces
-- it. served_at is set the first time it is handed out and cleared by a replacement: the device
-- is told its package was used and publishes a fresh one. The server never parses it.
--
-- A new, empty table. Its foreign key takes a SHARE ROW EXCLUSIVE lock on devices until the
-- commit, a moment: registrations and retirements wait for that moment, nothing is scanned.
-- 8192 is kMaxKeyPackageBytes.
CREATE TABLE last_resort_key_packages (
    device_id    uuid PRIMARY KEY REFERENCES devices (id),
    body         bytea NOT NULL,
    published_at timestamptz NOT NULL DEFAULT now(),
    served_at    timestamptz,
    CONSTRAINT last_resort_size CHECK (octet_length(body) BETWEEN 1 AND 8192)
);
