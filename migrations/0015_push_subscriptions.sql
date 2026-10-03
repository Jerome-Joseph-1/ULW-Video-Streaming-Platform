-- Web Push subscriptions, one per device of a user (ADR-0097): where the browser's push service
-- takes messages for the device, and the keys they are encrypted with (RFC 8291). chat_server
-- writes them when a client subscribes and reads a callee's when a call starts ringing. An
-- endpoint belongs to one device of one user; the push service's 404 or 410 deletes it.
CREATE TABLE push_subscriptions (
    user_id    text COLLATE "C" NOT NULL,
    device_id  uuid NOT NULL,
    endpoint   text COLLATE "C" NOT NULL,
    p256dh     bytea NOT NULL,
    auth       bytea NOT NULL,
    updated_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, device_id),
    -- chat_server checks all of these first (infra/webpush/endpoint.hpp); these keep anything
    -- else that writes here as honest.
    CONSTRAINT push_subscriptions_endpoint_https CHECK (endpoint LIKE 'https://%'),
    CONSTRAINT push_subscriptions_endpoint_length CHECK (octet_length(endpoint) <= 2048),
    CONSTRAINT push_subscriptions_p256dh_length CHECK (octet_length(p256dh) = 65),
    CONSTRAINT push_subscriptions_auth_length CHECK (octet_length(auth) = 16)
);
CREATE UNIQUE INDEX push_subscriptions_endpoint ON push_subscriptions (endpoint);

-- Saves a device's subscription in one statement: takes the endpoint from whoever held it,
-- writes the device's row, and forgets the user's devices updated longest ago past p_max. The
-- per-user advisory lock (class 15, the migration's number, so no other lock shares its keys)
-- keeps two saves of one user from each counting without the other and leaving p_max + 1.
CREATE FUNCTION push_subscribe(p_user text, p_device uuid, p_endpoint text, p_p256dh bytea,
                               p_auth bytea, p_max integer) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
    PERFORM pg_advisory_xact_lock(15, hashtext(p_user));
    DELETE FROM push_subscriptions
        WHERE endpoint = p_endpoint AND (user_id, device_id) <> (p_user, p_device);
    INSERT INTO push_subscriptions (user_id, device_id, endpoint, p256dh, auth)
        VALUES (p_user, p_device, p_endpoint, p_p256dh, p_auth)
        ON CONFLICT (user_id, device_id) DO UPDATE
            SET endpoint = EXCLUDED.endpoint, p256dh = EXCLUDED.p256dh, auth = EXCLUDED.auth,
                updated_at = now();
    DELETE FROM push_subscriptions
        WHERE user_id = p_user AND device_id IN (
            SELECT device_id FROM push_subscriptions WHERE user_id = p_user
                ORDER BY updated_at DESC, device_id DESC OFFSET p_max);
END
$$;
