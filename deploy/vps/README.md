# The platform on one host

`compose.yaml` runs the whole platform on a single Linux host as a product: one public origin,
`https://<public ip with dashes>.sslip.io`, with a Let's Encrypt certificate, users signing in
on a bundled Keycloak, and the gateway, worker, chat, live packager, LiveKit and egress from the
published images. It runs on the project's own host (the one with the self-hosted runners) in
`/opt/ulw` as compose project `ulw-prod`, deployed by `.github/workflows/deploy-vps.yml`.

| Path | What |
|---|---|
| `/` | the web app (`demo/web`, signing in on Keycloak) |
| `/api/` | the gateway: uploads, videos, playlists, live streams |
| `/rt` | chat's WebSocket (the page's `?token=` becomes the bearer header) |
| `/rtc`, `/whip` | LiveKit: call signalling and live ingest |
| `/ulw-media/` | the object store (MinIO), path-style, for signed URLs |
| `/auth/` | Keycloak: sign-in, tokens, key set, admin console (`/auth/admin/`) |

Published ports, and opened in ufw by the deploy when ufw is active: 80/tcp, 443/tcp, 443/udp
(HTTP/3), 7801/tcp (LiveKit ICE over TCP), 7802/udp (LiveKit's UDP mux), 3478/udp (TURN) and
5349/tcp (TURN over TLS). Postgres, MinIO, Redis, LiveKit's API, the gateway's webhook port and
Keycloak's database and management port have no host port.

## Signing in

The realm `ulw` holds the demo users `alice`, `bob` and `carol`, password `testtest123` (the
repository's fake-password convention; change them before anyone else relies on the host). The
web app uses the authorization code flow with PKCE as the public client `ulw-web`; one browser
holds one Keycloak session, so to be two users at once use two browsers or a private window.

Tokens carry `aud: ulw` (the client's audience mapper) and the user name as
`preferred_username`, which the services take as the user id (`ULW_JWT_SUBJECT_CLAIM`). They are
signed RS256; the services fetch Keycloak's key set from the public URL, which inside the stack
resolves to Caddy on loopback.

Keycloak's admin user is `admin`; its password was generated on the host at the first deploy:

    sudo cat /opt/ulw/.env        # KC_ADMIN_PASSWORD

That file holds every secret of the stack (database passwords, MinIO keys, the LiveKit key and
secret, chat's node secret). It is root's, mode 0600, never committed and never printed by the
workflow.

### Adding a user

In the admin console, `https://<host>/auth/admin/` (realm `ulw`): Users, Add user; set the user
name (letters, digits and `. _ - @`; it becomes the user's id on the platform), email, first and
last name, Email verified; Create; then Credentials, Set password, Temporary off. Or from the
host:

    sudo /opt/ulw/compose.sh exec keycloak bash -c '
      /opt/keycloak/bin/kcadm.sh config credentials --server http://localhost:8080/auth \
        --realm master --user admin --password "$KC_BOOTSTRAP_ADMIN_PASSWORD" &&
      /opt/keycloak/bin/kcadm.sh create users -r ulw -s username=dave -s enabled=true \
        -s email=dave@example.com -s firstName=Dave -s lastName=Demo -s emailVerified=true &&
      /opt/keycloak/bin/kcadm.sh set-password -r ulw --username dave --new-password "<password>"'

The page's contact and room lists come from `demo/web/rooms.json` (alice, bob and carol); a new
user can sign in, upload, watch and go live, and is reachable in chat once a room names them.

## Redeploy

Push to `deploy/vps`, or run the `deploy-vps` workflow by hand (Actions, deploy-vps, Run
workflow). It copies `compose.yaml`, the Caddyfile, the realm, the seccomp profile and the web
app from the checkout to `/opt/ulw`, keeps `.env`, pulls the pinned images, runs
`docker compose up -d`, waits for every service to be healthy, and runs the smoke test against
the public URL. To change the build, set the three image references in the workflow's `env`
(tag and digest, as `demo-release.yml` prints them).

From a checkout on the host, by hand:

    deploy/vps/deploy.sh "$(curl -fsS https://api.ipify.org)" <gateway image> <worker image> <chat image>

The realm is imported only when it does not exist yet: later changes to
`keycloak/ulw-realm.json.template` do not reach a running Keycloak; make them in the admin
console.

## Operate

Every command goes through `/opt/ulw/compose.sh`, which names the project, its files and env:

    sudo /opt/ulw/compose.sh ps
    sudo /opt/ulw/compose.sh logs -f --tail 100 gateway chat worker
    sudo /opt/ulw/compose.sh logs -f caddy keycloak livekit
    sudo /opt/ulw/compose.sh restart livekit
    sudo /opt/ulw/compose.sh down          # stops the stack, keeps the data
    sudo /opt/ulw/compose.sh up -d

Never run `docker system prune` or `docker volume prune` on this host: CI jobs and soaks use its
Docker too, and the stack's data lives in the volumes `ulw-prod_pg`, `ulw-prod_minio`,
`ulw-prod_keycloak-pg` and `ulw-prod_caddy-data`.

LiveKit reads the certificate for TURN over TLS when it starts. Caddy renews the certificate
about every 60 days; restart LiveKit afterwards (`restart livekit`, or any redeploy), or TURN
over TLS goes on presenting the old one until it expires. Calls over UDP, ICE over TCP and TURN
over UDP are unaffected.

## Back up

The platform's database, Keycloak's database and the object store:

    ts=$(date -u +%Y%m%dT%H%M%SZ); sudo install -d -m 0700 /var/backups/ulw
    sudo /opt/ulw/compose.sh exec -T postgres pg_dump -U ulw -Fc ulw | sudo tee /var/backups/ulw/ulw-$ts.dump >/dev/null
    sudo /opt/ulw/compose.sh exec -T keycloak-db pg_dump -U keycloak -Fc keycloak | sudo tee /var/backups/ulw/keycloak-$ts.dump >/dev/null
    sudo docker run --rm -v ulw-prod_minio:/data:ro -v /var/backups/ulw:/out \
      docker.io/library/postgres@sha256:1a6ab3f5345eb6dbe04a1349529caabdb0ab09293a09590fad07b2246bfa4b54 \
      tar -C /data -czf /out/minio-$ts.tar.gz .
    sudo cp /opt/ulw/.env /var/backups/ulw/env-$ts      # the secrets: keep it as private as the host

Restore: stop the stack (`down`), restore the dumps with `pg_restore --clean` into a running
`postgres` / `keycloak-db` (`up -d postgres keycloak-db` first), untar into the `ulw-prod_minio`
volume, put `.env` back, then `up -d`. Copy backups off the host.

## Host settings

The worker's and the packager's ffmpeg sandbox (`ulw_sandbox`) makes a user namespace. Ubuntu
24.04 can refuse that to unprivileged processes with
`kernel.apparmor_restrict_unprivileged_userns=1`; this host has it at `0` (and
`kernel.unprivileged_userns_clone=1`), which every deploy prints and never changes. If an upgrade
turns the restriction on, transcoding and live recordings fail in the sandbox; set it back with
`sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0` and persist it in
`/etc/sysctl.d/`, knowing that it loosens a hardening for every process on the host.

The containers run with the seccomp profile `/opt/ulw/seccomp.json` (Docker's default plus
`unshare`, `mount` and `mount_setattr`), `systempaths=unconfined` and `apparmor=unconfined`, as
the Kubernetes pods get `procMount: Unmasked`; only the worker and the gateway (whose live
packager children run the same sandbox).

Docker publishes ports through its own iptables rules, ahead of ufw: the ufw rules the deploy
adds document what is open rather than gate it.

## Limits

- One host: one gateway, worker and chat node, one live stream at a time (the process packager
  runtime), all sharing the CPU with CI jobs and soaks.
- linux/amd64 images only.
- MinIO's root key serves the gateway, the worker and the packager alike; the Kubernetes
  deployment gives each component its own token (ADR-0066).
- The page's directory of people and rooms is the demo's (`rooms.json`).
