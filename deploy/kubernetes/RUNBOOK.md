# Deploying ULW on Kubernetes

For an operator: the person who runs ULW on their own cluster, with their own identity
provider, database and object store. Nothing in this repository applies these files to any
cluster; every command below is yours to run. Run a staging environment first, then production
the same way a release later.

You never build anything: the images are published by this repository, public, and you pull
them (step 4). Everything that differs from one operator to the next (namespace, hostname,
Gateway, identity provider, object store, image tags, TURN port, secret names) lives in one
file per environment, the overlay's `config.env` (step 4). Secrets live in Kubernetes Secrets
that you create (step 3), never in that file.

What ships:

| Path | What |
|---|---|
| `base/video-gateway/` | Deployment (2 replicas, migrations in its init container), Service, HTTPRoute + BackendTrafficPolicy, NetworkPolicy |
| `base/video-worker/` | Deployment (1 replica), NetworkPolicy; no Service, no route |
| `base/upload-reaper/` | CronJob (every 15 minutes) and NetworkPolicy; the gateway image's `ulw_reaper` (step 3a) |
| `base/chat/` | Deployment (3 replicas, docs/adr/0019), Service, HTTPRoute for its WebSocket (`/rt`), NetworkPolicy |
| `base/live-packager/` | The headless Service that names each stream's packager in DNS, and the NetworkPolicy every packager runs under (step 9) |
| `base/stunner/` | The TURN Gateway (UDP `TURN_PORT`) and the UDPRoute to LiveKit |
| `base/livekit/` | LiveKit (1 replica), Service, HTTPRoute for its signalling (`/rtc`) and WHIP (`/whip`), NetworkPolicy |
| `base/livekit-redis/` | Redis (1 replica, nothing persisted), ClusterIP Service, NetworkPolicy: LiveKit's bus to egress |
| `base/livekit-egress/` | LiveKit's recorder (1 replica), NetworkPolicy; no Service: it relays live streams to their packagers (step 9) |
| `components/operator-config/` | The kustomize component that copies each `config.env` value into the manifests that use it |
| `overlays/staging/` | An example environment that follows the `main` build: `kustomization.yaml` and its `config.env` |
| `overlays/production/` | An example environment pinned to a published commit, with room for a larger worker and two live streams |
| `cluster/stunner/` | The STUNner gateway operator, its dataplane template, the GatewayClass and GatewayConfig: once per cluster (step 7) |
| `cluster/seccomp/ulw-worker.json` | The worker's seccomp profile, installed on each node (step 2) |
| `live-packager/job.yaml` | One stream's packager, a Job made from this template per stream; in no kustomization, so applying an overlay never creates one (step 9, docs/adr/0083) |

The base carries example values (namespace `ulw`, `video.example.com`, `id.example.com`) only so
that it is a valid set of manifests; an overlay replaces every one of them from its
`config.env`, and a key missing from that file fails the build. `deploy/local` runs the same
base in a kind cluster with a `config.env` of its own (`make e2e-up`), which is the closest thing
to a rehearsal this repository offers.

Live streams also need a stream service to start each stream's packager, which does not exist
yet: until it does, a packager is started by hand (step 9).

## 1. Check the cluster

ULW needs:

- Kubernetes v1.33 or later, with pods in user namespaces (below);
- [Envoy Gateway](https://gateway.envoyproxy.io) v1.x and a Gateway API `Gateway` the routes
  attach to (`GATEWAY_NAME` in `GATEWAY_NAMESPACE`), with a listener for `PUBLIC_HOSTNAME` that
  terminates TLS. The gateway's route uses Envoy Gateway's `BackendTrafficPolicy`; another
  Gateway API implementation needs that resource replaced;
- a CNI that enforces NetworkPolicy, since every pod's policy is part of its security;
- Postgres 16, an object store (Cloudflare R2, or any S3-compatible store), and an identity
  provider that signs JWTs and publishes a JWKS (docs/integration/auth.md), all reachable from
  the pods;
- for calls and live streams, a public UDP port for TURN (step 7).

The worker gives each ffmpeg its own namespaces (docs/adr/0032), which needs pods in user
namespaces. On each node:

```sh
kubectl version                        # server v1.33 or later
uname -r                               # 6.3 or later (idmapped mounts on overlayfs)
containerd --version; runc --version   # containerd 2.x, runc 1.2 or later
```

Two host settings stop the worker on otherwise capable machines; GitHub's Ubuntu 24.04 runners
have both. On each node, as an ordinary user (not root, not sudo):

```sh
sysctl kernel.apparmor_restrict_unprivileged_userns   # 0, or "unknown key"
unshare --user --map-root-user --net --mount --pid --fork --mount-proc true && echo ok
unshare --user --map-root-user --net --mount \
  sh -c 'mount -t sysfs -o ro,nosuid,nodev,noexec sysfs /mnt && echo ok'
```

- `apparmor_restrict_unprivileged_userns = 1` (Ubuntu 23.10 and later) lets a process with no
  capabilities on the host create a user namespace but not use it, and the worker is such a
  process. The first `unshare` then fails with `write failed /proc/self/uid_map: Operation not
  permitted`, and so would the worker's start-up check. The e2e workflow sets it to `0` on its
  runner for this reason. The nodes need the same unless they run a kernel without the key:
  `echo kernel.apparmor_restrict_unprivileged_userns=0 | sudo tee /etc/sysctl.d/60-ulw-userns.conf`
  then `sudo sysctl --system`. It is host-wide; the alternative, an AppArmor profile for the
  worker that allows `userns`, has not been written or tested.
- If the sysfs mount fails, a file is mounted over part of the node's `/sys`
  (`grep ' /sys/' /proc/self/mountinfo` shows it). The kernel then refuses a fresh sysfs in any
  user namespace, and every `hostUsers: false` pod stays in `ContainerCreating` with "error
  mounting sysfs ... operation not permitted". kind does this to its nodes on any VM with DMI
  (the sandbox undoes it in deploy/local/e2e-up.sh); an ordinary node normally has no such
  mount.

One difference the sandbox cannot show: containerd inside a kind node never applies AppArmor,
while a node where `cat /sys/module/apparmor/parameters/enabled` prints `Y` usually gives every
pod its default AppArmor profile, which denies `mount`. On such a node watch the worker's first
start; a `mount /proc` error in its log means the worker needs its own AppArmor profile
(`securityContext.appArmorProfile`) before it can run.

The gateway's and chat's NetworkPolicies admit only Envoy's data plane, found by labels, in
Envoy Gateway's default layout. Confirm them, or patch `video-gateway` and `chat`'s
NetworkPolicies in your overlay before the first apply:

```sh
kubectl get pods -A -l app.kubernetes.io/component=proxy,app.kubernetes.io/managed-by=envoy-gateway \
  -o custom-columns=NS:.metadata.namespace,NAME:.metadata.name   # expect namespace envoy-gateway-system
```

They also admit Prometheus from namespace `monitoring` to scrape `/metrics`. If Prometheus runs
elsewhere, patch the second `from` of the same policies:

```sh
kubectl get pods -A -l app.kubernetes.io/name=prometheus -o custom-columns=NS:.metadata.namespace
```

The gateway's, chat's, the worker's, the reaper's and the packagers' NetworkPolicies also limit
what they reach out to: cluster DNS in `kube-system`, TCP 5432 (Postgres) and 443 (the object
store, and for the gateway and chat the identity provider's JWKS). The worker's and the
packagers' 5432 and 443 exclude the pod network, `POD_CIDR` in `config.env`. The gateway's 443
does not, in case `JWKS_URL` names an in-cluster Service. A policy matches the pod's own port,
so if Postgres, the store (an in-cluster MinIO on 9000, say) or the JWKS is served on another
port, patch it into the egress rules in your overlay, or the store fails every job or the gateway
fetches no keys (`auth_failures` rise with 503s); deploy/local/cluster/networkpolicy-patch.yaml
does exactly that for the sandbox. Check, before the first apply:

```sh
kubectl get pods -n kube-system -l k8s-app=kube-dns -o name        # cluster DNS is in kube-system
kubectl get nodes -o jsonpath='{.items[*].spec.podCIDR}'           # each inside POD_CIDR
```

After it, the gateway's log shows no failed key fetch and its `/readyz` stays 200; a worker
that cannot reach the bucket fails its first job with a store error.

The gateway limits each client address (20 connections, or 20 requests in flight before they
are authenticated, and 10 new connections a second) and each user (300 requests a minute, 100
GiB of uploads a day), per replica. Behind Envoy every connection comes from Envoy's pods, so
the gateway takes the client address from `X-Forwarded-For`, from peers in `ULW_TRUSTED_PROXIES`
only, and only the entry the `ULW_TRUSTED_PROXY_HOPS` proxies in front appended: with Envoy
alone, the last one. The base sets `ULW_TRUSTED_PROXIES` to `POD_CIDR` and one hop. Confirm
Envoy's pods are in that block, and that Envoy sees clients' own addresses rather than a node's:
its Service must have `externalTrafficPolicy: Local`, or kube-proxy rewrites every client to a
node address before Envoy appends it.

```sh
kubectl get pods -n envoy-gateway-system -l app.kubernetes.io/component=proxy \
  -o custom-columns=NAME:.metadata.name,IP:.status.podIP
kubectl get svc -n envoy-gateway-system -l app.kubernetes.io/component=proxy \
  -o custom-columns=NAME:.metadata.name,POLICY:.spec.externalTrafficPolicy   # Local
```

With the wrong block every client counts as Envoy: the gateway resets Envoy's connections past
20 and `connections_rejected_total{reason="ip_connections"}` climbs. With a proxy more or fewer
in front than `ULW_TRUSTED_PROXY_HOPS` says (a CDN before Envoy, say), clients are counted as
the wrong address; patch the hop count into the gateway and chat, and step 5.4 checks which
address the gateway sees.

`chat_server` holds each client address to a share of its connections too (20 open sockets, 80
from one IPv6 /48, 10 new ones a second, and 16 sockets per user; docs/adr/0076), and reads the
same `ULW_TRUSTED_PROXIES` and `ULW_TRUSTED_PROXY_HOPS`. Without them every WebSocket arrives
from Envoy's pods, so all clients together share Envoy's 20 sockets per address: the 21st client
through an Envoy pod is reset at accept, and `connections_rejected_total{reason="ip_connections"}`
on the chat node climbs while its `connections_current` is nowhere near 1280, the node's fixed
cap. With the proxies set, the chat node tells clients apart by `X-Forwarded-For` at their
upgrade requests, and it refuses `ULW_TRUSTED_PROXY_HOPS` without `ULW_TRUSTED_PROXIES` and warns
of a block wider than /8 (IPv6 /32), as the gateway does.

The NetworkPolicy admits Envoy's pods and every pod in the `monitoring` namespace, both from
inside the trusted block. A monitoring pod can therefore send any `X-Forwarded-For` it likes; all
that buys it is choosing which address its own unauthenticated requests are counted against,
and it holds no token to do more.

The other limits are environment variables (`ULW_MAX_CONNECTIONS_PER_IP`,
`ULW_NEW_CONNECTIONS_PER_IP_PER_SECOND`, `ULW_REQUESTS_PER_USER_PER_MINUTE`,
`ULW_UPLOAD_BYTES_PER_USER_PER_DAY`), listed with their ranges in
`docs/integration/operator-contract.md`, to patch in an overlay; the defaults and their
derivations are in docs/adr/0052. Each applies per replica, so with two replicas a user may reach
twice a per-user limit. The byte quota is best effort: it lives in each replica's memory, is
forgotten on a restart, and a user unseen while 16,384 others were active starts over.

The pods start as user 10001 and have nothing to drop. A process started as root (a
hand-started binary, a supervisor that stays root) must be given `ULW_RUN_AS_USER`, which it
becomes after binding its port and before it serves, or it refuses to start with exit 2;
`ULW_ALLOW_ROOT=1` lets it stay root, for development only.

Check the nodes have room. Per environment the base requests 2 x 500m CPU and 2 x 600Mi for the
gateways, the worker's 2Gi with 1 CPU and 10Gi of scratch (`overlays/production`: 2 CPU and
30Gi), and 3 x 250m CPU and 3 x 832Mi for chat: about 3 CPU, 5.6Gi of memory and 10 to 30Gi of
ephemeral storage. The realtime plane adds about 1 CPU and 450Mi, and egress 1 CPU and 512Mi
(step 7). Each live stream adds a packager's 100m CPU, 256Mi of memory and 256Mi of ephemeral
storage while it runs (step 9). Compare with what is already allocated:

```sh
kubectl describe nodes | sed -n '/Allocated resources/,/Events/p'
df -h /var/lib/kubelet                 # the scratch emptyDirs live here
```

The gateways' memory requests are the budget of docs/adr/0027 and chat's the worst case of
docs/adr/0036; neither should move. On a single node the three chat pods share one failure
domain: ADR-0019's point about a drain taking two of them at once becomes all three, and no
PodDisruptionBudget is shipped, since on one node it would only stop the drain.

Check that the JWKS host speaks TLS 1.3, and the store's if `S3_ENDPOINT` is `https://`. The
gateway, the worker and chat refuse any https server that does not
(docs/integration/operator-contract.md), so a JWKS host stuck on TLS 1.2 leaves every request
`503` until it is fixed. From any machine:

```sh
host=id.example.com                    # the host of JWKS_URL
openssl s_client -tls1_3 -connect "$host:443" -servername "$host" </dev/null \
    2>&1 | grep -E '^ *Protocol *:|New, TLSv1.3'
```

It must print TLSv1.3; a handshake failure means the host has to enable TLS 1.3 before this
deploys. R2 needs no check: Cloudflare serves TLS 1.3.

## 2. Install the worker's seccomp profile

Once per node that may run the worker or a packager, and again whenever
`cluster/seccomp/ulw-worker.json` changes:

```sh
sudo install -D -m 0644 cluster/seccomp/ulw-worker.json /var/lib/kubelet/seccomp/profiles/ulw-worker.json
```

Without it the worker pod stays in `CreateContainerError`, and so does every live packager
(step 9), which runs ffmpeg under the same sandbox and profile.

## 3. Secrets and the database

Each environment's Secrets live in its `NAMESPACE`, under the names its `config.env` gives
(`VIDEO_GATEWAY_SECRET` and the rest; the defaults are below). Only secret values go in them:
the store's account id or endpoint and the bucket are in `config.env`. The bucket needs the
lifecycle rule of step 3a; the gateway expires its uploads after 6 days.

```sh
NS=ulw                                  # NAMESPACE in config.env
kubectl create namespace "$NS"
# One store token per component (docs/adr/0066): object read and write on the bucket each.
kubectl -n "$NS" create secret generic video-gateway-secrets \
  --from-literal=ULW_DATABASE_URL="$DATABASE_URL" \
  --from-literal=ULW_S3_ACCESS_KEY_ID="$GATEWAY_STORE_KEY_ID" \
  --from-file=ULW_S3_SECRET_ACCESS_KEY=<(printf '%s' "$GATEWAY_STORE_SECRET") \
  --dry-run=client -o yaml | kubectl apply -f -
kubectl -n "$NS" create secret generic video-worker-secrets \
  --from-literal=ULW_DATABASE_URL="$DATABASE_URL" \
  --from-literal=ULW_S3_ACCESS_KEY_ID="$WORKER_STORE_KEY_ID" \
  --from-file=ULW_S3_SECRET_ACCESS_KEY=<(printf '%s' "$WORKER_STORE_SECRET") \
  --dry-run=client -o yaml | kubectl apply -f -
# ULW_NODE_SECRET: 32+ random bytes, the same in every chat pod (docs/adr/0035).
kubectl -n "$NS" create secret generic chat-secrets \
  --from-literal=ULW_DATABASE_URL="$DATABASE_URL" \
  --from-file=ULW_NODE_SECRET=<(openssl rand -base64 48 | tr -d '\n') \
  --dry-run=client -o yaml | kubectl apply -f -
```

`DATABASE_URL` is `postgresql://<role>:<password>@<host>:5432/<database>`, the password
percent-encoded; how you hold these values (a password manager, sealed-secrets, an external
secrets operator) is yours, as long as the Secrets end up with these keys. The worker gets no
JWT settings at all. Chat uses the gateway's database and role: its tables come from the same
migrations, which the gateway's init container applies (docs/adr/0031).

A changed Secret reaches the pods only through a restart:

```sh
kubectl -n "$NS" rollout restart deployment/video-gateway deployment/video-worker deployment/chat
```

The chat nodes refuse each other unless they hold the same `ULW_NODE_SECRET`, so a new value
must reach every pod: the restart above rolls them one at a time, and while old and new pods
overlap, rooms owned across the two are unreachable from the other side and their joins and
sends fail as `unavailable` (clients retry). To rotate it without that, scale chat to 0 and back
to 3 instead of the restart.

The role and database on your Postgres, once per environment, as a superuser (psql prompts for
the password with `\password`; it never goes on a command line):

```sql
CREATE ROLE ulw LOGIN;
\password ulw
CREATE DATABASE ulw OWNER ulw;
```

The role owns its database, which gives the migrations their DDL rights (docs/adr/0031).

Chat message bodies travel as bound parameters, which the server writes to its log whenever it
logs a statement with its parameters or an error in one (docs/adr/0054). Keep them out, on the
same database, as the same superuser, whatever statement logging is on now or later:

```sql
ALTER DATABASE ulw SET log_parameter_max_length = 0;
ALTER DATABASE ulw SET log_parameter_max_length_on_error = 0;
-- Only where auto_explain is loaded:
ALTER DATABASE ulw SET auto_explain.log_parameter_max_length = 0;
```

`SHOW log_parameter_max_length;` and `SHOW log_parameter_max_length_on_error;` in a new session
as `ulw` then print `0`.

Chat rooms other than a stream's live chat admit only their listed members (docs/adr/0054).
Until the product manages the lists, they are rows in `chat_members`, set as the service's role.
Record the room as closed in the same transaction, before its first member, as the service's own
statement does, so that it can never be recorded live while it lists anyone:

```sql
BEGIN;
INSERT INTO chat_rooms (room_id, kind) VALUES ('<room uuid>', 'group_chat')
ON CONFLICT (room_id) DO NOTHING;
INSERT INTO chat_members (room_id, user_id) VALUES ('<room uuid>', '<user id>')
ON CONFLICT (room_id, user_id) DO NOTHING;
COMMIT;

DELETE FROM chat_members WHERE room_id = '<room uuid>' AND user_id = '<user id>';
```

`<user id>` is the token's subject claim (`JWT_SUBJECT_CLAIM`, docs/integration/auth.md). A
member removed this way is cut off at once on every chat node, however the row goes (a DELETE,
or an UPDATE that moves it to another room or user; one that leaves both as they were removes
nobody): a trigger (migration 0009) notifies the nodes, each takes that user's sockets out of the
room, and the client gets an `error` with `not_member` for it (docs/adr/0073). Their next join is
refused. No restart is needed. If a node's listening session to Postgres was down when the row
went, the node checks every closed room its clients are in once it listens again, four checks at
a time and retrying each second while the database fails, so a removal made during a database
outage takes effect once the node reconnects. `member_removals_total` counts the sockets taken out.

A stream's live chat admits anyone, and only the server side opens one: a client's join can
record a room only as closed, and a stream join is refused with `not_live` until the stream's
chat is open. A stream's chat room is named by the stream (docs/adr/0070), and viewers join it
by the stream's name. Only such a room can be live: the database refuses any other id
(`chat_rooms_live_is_a_stream`), since every chat node tells a live chat by its id alone. Until
the product calls `IMessageStore::record_live` when a stream goes on air, open a stream's chat
before its viewers arrive, as the service's role, with the stream's name for `<stream>`:

```sql
INSERT INTO chat_rooms (room_id, kind)
SELECT live_chat_room('<stream>'), 'stream_live_chat'
 WHERE NOT EXISTS (SELECT 1 FROM chat_members WHERE room_id = live_chat_room('<stream>'))
   AND NOT EXISTS (SELECT 1 FROM room_state
                    WHERE room_id = live_chat_room('<stream>') AND kind <> 'stream_live_chat')
ON CONFLICT (room_id) DO UPDATE SET kind = chat_rooms.kind
RETURNING kind;
```

It must print `stream_live_chat`. Anything else (`group_chat`, `direct_chat`, or no row, when the
room lists members or was already created closed) means the room is closed, and stays so: a
recorded kind never changes, so a room that was joined, listed or created before it was opened,
including every room from before M19 (migration 0006), needs a new room id.

The chat nodes speak a versioned channel to each other (docs/adr/0043), and a node refuses a
peer of another version. A release that changes the version (M19 moves it from 2 to 3, the
call handler from 3 to 4, docs/adr/0087, and the call's ring from 4 to 5, docs/adr/0091; one
`Recreate` covers both when they ship together) splits a rolling update in two: until the last old pod
is gone, old and new nodes cannot reach each other, rooms owned across the split are unreachable
from the other side, and their joins and sends fail as `unavailable` (clients retry them). Roll
such a release out with the chat Deployment's strategy set to `Recreate`
(`spec.strategy: {type: Recreate}`, patched in your overlay for that release), which stops every
old pod before starting the new ones: a short full outage instead of a split one. Releases that
keep the version roll as usual.

### 3a. Lifecycle rule and upload reaper

The bucket aborts incomplete multipart uploads under `videos/` after 7 days, a day past the
gateway's 6-day upload lifetime. This is the backstop; the reaper below normally gets there
within minutes of an upload expiring (docs/adr/0049). Once per bucket, with a token that has
admin read and write on the bucket (neither the gateway's nor the worker's), and the AWS CLI:

```sh
cat > lifecycle.json <<'JSON'
{
  "Rules": [
    {
      "ID": "abort-incomplete-multipart-videos",
      "Status": "Enabled",
      "Filter": { "Prefix": "videos/" },
      "AbortIncompleteMultipartUpload": { "DaysAfterInitiation": 7 }
    }
  ]
}
JSON
export AWS_ACCESS_KEY_ID=… AWS_SECRET_ACCESS_KEY=… AWS_DEFAULT_REGION=auto
STORE="https://$R2_ACCOUNT_ID.r2.cloudflarestorage.com"   # or S3_ENDPOINT
aws s3api get-bucket-lifecycle-configuration --endpoint-url "$STORE" --bucket "$BUCKET"
aws s3api put-bucket-lifecycle-configuration --endpoint-url "$STORE" --bucket "$BUCKET" \
  --lifecycle-configuration file://lifecycle.json
aws s3api get-bucket-lifecycle-configuration --endpoint-url "$STORE" --bucket "$BUCKET"
```

The put replaces the bucket's whole lifecycle configuration: if the first get shows rules, merge
them into `lifecycle.json`. On R2 the same rule is in the dashboard under the bucket's Settings,
Object lifecycle rules, as "Abort incomplete multipart uploads" with prefix `videos/` and 7 days.
A store that refuses `AbortIncompleteMultipartUpload` in a lifecycle (MinIO, which has a
server-wide setting for it instead) relies on the reaper alone.

The reaper is `base/upload-reaper/`: a CronJob running the gateway image's `ulw_reaper` every 15
minutes with the gateway's secret (the same image as the gateway, so the reaper's SQL and lock
key match the build the gateways run), and a NetworkPolicy that lets it reach cluster DNS,
Postgres (5432) and the store (443) and nothing else. It aborts uploads past their `expires_at`,
fails their videos with "upload expired", releases their storage sessions, removes any object a
finished commit left at their key, and aborts sessions older than the uploads' lifetime that no
upload owns (docs/adr/0049). It also forgets direct and group chat rooms that a refused join
recorded more than a day ago and nothing used since (no members, never on the room plane),
however old; a stream's live chat is never forgotten. It looks at 10,000 rooms a pass at most,
from where the last pass stopped, and starts over from the oldest once it reaches the cutoff
(`chat_rooms_forget_cursor`, docs/adr/0075). Each pass prints
`reaper_uploads_expired_last_run`, `reaper_uploads_release_failed_last_run`,
`reaper_parts_orphaned_last_run` and `reaper_chat_rooms_forgotten_last_run` on stdout, as
gauges; a non-zero exit, so a failed Job, means a phase failed or an upload's release was not
confirmed, and the Job's log says which.

**Deploy the release that carries migration 0010 off-peak.** Migrations run inside a
transaction, so its index on `chat_rooms (recorded_at, room_id)` is built without
`CONCURRENTLY`: until the gateway's migrate container commits it, every chat join of a closed
room, every member listing and every live chat opened waits (the column it adds is a catalog
change, with no rewrite). The build sorts every row of `chat_rooms`, a uuid, a text and a
timestamp each: a few seconds per million rooms, so check first with
`SELECT count(*) FROM chat_rooms;`. Taking its locks waits at most 5 s for traffic before the
migration gives up and the init container runs it again (`lock_timeout`, docs/adr/0031). A join
that waits past the chat service's request timeout is answered `unavailable`, and the client
retries it.

The NetworkPolicies allow ports, not addresses, because Postgres and the store often run outside
the cluster. If their addresses are stable, patch them in as an `ipBlock` on the 5432 and 443
rules.

## 4. The operator config and the first deploy

### The images

The images come prebuilt from GitHub's container registry, public, so no pull secret is
needed; the nodes need to reach `ghcr.io` and `pkg-containers.githubusercontent.com` over https.
`.github/workflows/publish-images.yml` in this repository builds the four Dockerfile targets
for every push to `main`, checks them as the e2e sandbox does (the release binaries' hardening
and Trivy's image gate; either failing stops it), waits for that commit's `ci` run to succeed,
and only then pushes:

| Image | Dockerfile target | Runs | `config.env` key |
|---|---|---|---|
| `ghcr.io/jerome-joseph-1/ulw-video-gateway` | `gateway` | video-gateway (and its migrate init container), upload-reaper | `VIDEO_GATEWAY_IMAGE_TAG` |
| `ghcr.io/jerome-joseph-1/ulw-video-worker` | `worker` | video-worker | `VIDEO_WORKER_IMAGE_TAG` |
| `ghcr.io/jerome-joseph-1/ulw-chat` | `chat` | chat | `CHAT_IMAGE_TAG` |
| `ghcr.io/jerome-joseph-1/ulw-live-packager` | `live-packager` | each stream's Job (step 9) | `LIVE_PACKAGER_IMAGE_TAG` |

Each is tagged with the full commit SHA and with `main`, never `latest` (docs/adr/0085). A SHA
tag is pushed once and always resolves to the same digest: publishing a commit again reuses
what was published. `main` moves for all four images together, only after all four SHA tags
are pushed, and only to main's tip. The run's summary lists each image as
`ghcr.io/jerome-joseph-1/ulw-<service>:<sha>@sha256:<digest>`, ready to pin (4a). Each digest
carries a build provenance attestation from the run that first pushed it
(`gh attestation verify oci://ghcr.io/jerome-joseph-1/ulw-chat@sha256:... --repo
Jerome-Joseph-1/ULW-Video-Streaming-Platform`). LiveKit, its egress, Redis and STUNner are
upstream images, pinned by digest in the manifests.

Not every commit on main is published: a newer push replaces a waiting publish, and a publish
stops if the commit's `ci` run fails. A commit whose run succeeded is published from Actions,
publish-images, "Run workflow" (from `main`) with its SHA as `ref`, by the repository's
maintainers.

### The operator config: `config.env`

Copy `overlays/staging` (an environment that follows `main`) or `overlays/production` (one
pinned to a published commit) to a directory of your own, one per environment, and fill in its
`config.env`. That one file is the whole of an environment's operator configuration; every key
is required (a missing one fails the build), and every value shipped is an example:

| Key | What | Used as |
|---|---|---|
| `NAMESPACE` | The namespace everything runs in | every resource's namespace; `<namespace>` in the packagers' DNS names |
| `GATEWAY_NAME`, `GATEWAY_NAMESPACE` | Your Gateway API Gateway | the HTTPRoutes' `parentRefs` |
| `PUBLIC_HOSTNAME` | The host clients reach: the web app's own, so `/api`, `/rt` and `/rtc` are same-origin | the HTTPRoutes' `hostnames` |
| `POD_CIDR` | The cluster's pod network | `ULW_TRUSTED_PROXIES` of the gateway and chat; the block the worker and packagers may not reach |
| `JWKS_URL`, `JWT_ISSUER`, `JWT_AUDIENCE` | The identity provider (docs/integration/auth.md) | the gateway's and chat's settings of the same names |
| `JWT_SUBJECT_CLAIM` | The claim that names the user, `sub` unless your provider uses another | `ULW_JWT_SUBJECT_CLAIM` |
| `AUTH_COOKIE`, `ALLOWED_ORIGINS` | The token cookie, and the web app's pages that may use it | `ULW_AUTH_COOKIE`, `ULW_ALLOWED_ORIGINS` |
| `STORAGE`, `R2_ACCOUNT_ID`, `S3_ENDPOINT`, `BUCKET` | The object store: `r2` with an account id, or `minio` (any S3-compatible store) with an endpoint; the unused one empty | `ULW_STORAGE` and the rest, for the gateway, worker, reaper and packagers |
| `VIDEO_GATEWAY_IMAGE_TAG`, `VIDEO_WORKER_IMAGE_TAG`, `CHAT_IMAGE_TAG`, `LIVE_PACKAGER_IMAGE_TAG` | Which build runs: `main`, a commit SHA, or `<sha>@sha256:<digest>` (4a) | each image's tag |
| `IMAGE_PULL_POLICY` | `Always` for a moving tag, `IfNotPresent` for a pinned one | the four images' pull policy |
| `TURN_PORT` | STUNner's public UDP port (step 7) | the TURN listener, and the port LiveKit hands clients |
| `VIDEO_GATEWAY_SECRET`, `VIDEO_WORKER_SECRET`, `CHAT_SECRET`, `SFU_SECRET`, `LIVE_PACKAGER_SECRET` | The names of the Secrets of step 3, 7 and 9 | every `secretKeyRef` |

The file is plain `KEY=value` lines, no quotes and no spaces, so a shell can source it too (the
packager's Job in step 9 is filled from it). `components/operator-config` copies each value
into the manifests; read it to see exactly where. What `config.env` does not cover (replica
counts, resources, the per-client limits, an extra hostname, a hop count) you change with
kustomize patches in your overlay, as `overlays/production` patches the worker's and egress's
resources.

Your overlay can live in your own repository instead, against a pinned commit of this one:

```yaml
resources:
  - https://github.com/Jerome-Joseph-1/ULW-Video-Streaming-Platform//deploy/kubernetes/base?ref=<sha>
components:
  - https://github.com/Jerome-Joseph-1/ULW-Video-Streaming-Platform//deploy/kubernetes/components/operator-config?ref=<sha>
```

with the same `configMapGenerator` as the shipped overlays. Use the same `<sha>` for both and
for the image tags, so the manifests and the images are one release.

### First deploy

Render and check before applying; the output is everything the environment runs:

```sh
kubectl kustomize overlays/<env> > /tmp/ulw.yaml
grep -n 'image:' /tmp/ulw.yaml                       # your tags, upstream images by digest
../local/check-image-pins.py --pinned /tmp/ulw.yaml   # a production overlay: no :main (4a)
kubectl apply --dry-run=server -f /tmp/ulw.yaml
kubectl apply -k overlays/<env>
kubectl -n "$NS" rollout status deployment/video-gateway --timeout=10m
```

The gateway's init container migrates the schema before its pods serve, so start with it on a
new database; the worker and chat wait for its tables. A GitOps tool (Argo CD, Flux) can apply
the same overlay; it then owns the manifests, and a `kubectl set image` or `kubectl set env` by
hand is undone at its next sync.

To run a newer build of an environment that follows `main`, restart, gateway first (its init
container migrates the schema before the new pods serve):

```sh
kubectl -n "$NS" rollout restart deployment/video-gateway
kubectl -n "$NS" rollout status deployment/video-gateway --timeout=10m
kubectl -n "$NS" rollout restart deployment/video-worker deployment/chat
```

A pinned environment is not restarted onto a new build: its deploy is a change of the image
tags in its `config.env` (4a) and an apply.

The routes serve `/api/v1/uploads`, `/api/v1/videos` and `/api/v1/live` (the gateway), `/rt`
(chat), and `/rtc` and `/whip` (LiveKit) on `PUBLIC_HOSTNAME` only. Environments often attach to
one shared Gateway, so a route without `hostnames:` would answer the other environments' hosts
too. To serve a second host, patch it into the three HTTPRoutes' `spec.hostnames`.

### 4a. Deploying by digest

`main` is mutable: each publish moves it, so a restart, reschedule or node drain can change what
runs, and two gateway pods started a minute apart can run different builds. Accept that on a
staging environment only. A production environment always names a published commit: its SHA
tag, which publish-images never moves once pushed, and preferably its digest too, as
`<sha>@sha256:<digest>`, which the runtime pulls by digest while the SHA keeps it readable.

`:main` (or any other moving tag) in a production overlay is caught by
`deploy/local/check-image-pins.py --pinned`, which fails if a rendered overlay names one of
these images by anything but a 40-hex SHA or a digest. This repository runs it on
`overlays/production`; nothing runs it on yours, so run it on your own production overlay before
every apply, from a checkout of this repository, from `deploy/kubernetes` as every command here
(it needs Python 3 and PyYAML):

```sh
kubectl kustomize overlays/<env> > /tmp/ulw.yaml
../local/check-image-pins.py --pinned /tmp/ulw.yaml    # exit 0, or the image at fault
```

1. Take the images from the publish run's summary for the commit you mean to deploy (Actions,
   publish-images, the run, Summary, "Published images"), each as `<sha>@sha256:<digest>`, or
   resolve them from its SHA tag; both give the same `sha256:...`:

   ```sh
   SHA=<full commit sha on main>
   for svc in video-gateway video-worker chat live-packager; do
     echo "$svc $SHA@$(crane digest "ghcr.io/jerome-joseph-1/ulw-$svc:$SHA")"
   done
   ```

2. Set each in the environment's `config.env`, with `IMAGE_PULL_POLICY=IfNotPresent` (a digest
   never changes, so there is nothing to pull again):

   ```
   VIDEO_GATEWAY_IMAGE_TAG=<sha>@sha256:<gateway digest>
   VIDEO_WORKER_IMAGE_TAG=<sha>@sha256:<worker digest>
   CHAT_IMAGE_TAG=<sha>@sha256:<chat digest>
   LIVE_PACKAGER_IMAGE_TAG=<sha>@sha256:<live-packager digest>
   ```

   Apply the overlay; the gateway's init container migrates first, as on every rollout. A deploy
   is then a change of these four lines, and a rollback (step 6) a change back.
3. The live packager's Job is not in the overlay: whatever starts a stream names its image, from
   the same `LIVE_PACKAGER_IMAGE_TAG` (step 9).

On an environment that follows `main`, what a pod runs is at least recorded. This prints each
pod's resolved digest, which must match the summary's digest for the commit you meant to deploy:

```sh
kubectl -n "$NS" get pods -o \
  jsonpath='{range .items[*]}{.metadata.name}{" "}{.status.containerStatuses[*].imageID}{"\n"}{end}'
```

### For maintainers: image builds and the worker's ffmpeg

The images come from two distributions (docs/adr/0074): `video-gateway` and `chat` are Ubuntu
24.04 with their packages from snapshot.ubuntu.com (chat adds the jemalloc it links,
docs/adr/0081), `video-worker` and `live-packager` are Debian 13 (trixie) with their packages,
ffmpeg among them, from snapshot.debian.org, one stage shared by both; every image's binaries
are built on Ubuntu, since only Ubuntu's glibc lets them carry the CET marks the hardening
check requires. On the Debian images those marks are a static property only: Debian's own
`libc.so.6` is unmarked, so the kernel does not enable a shadow stack for their processes. And
because their binaries run on trixie's libraries, the Ubuntu release they are built on must not
have a newer glibc or libstdc++ than trixie; the trixie workflow fails if they stop loading
there. publish-images builds on GitHub's hosted runners, which reach both snapshot services
over https, deb.debian.org over http for the Debian stage's bootstrap of ca-certificates (signed
and checked for freshness), and Docker Hub for both base images. A builder elsewhere behind a
TLS-inspecting proxy passes its CA as the build secret `ca-bundle`. Each image is one `target:`
of `deploy/docker/Dockerfile` (`gateway`, `worker`, `chat`, `live-packager`); a bump of the
worker's ffmpeg below reaches the live packager's image too.

#### Updating the worker's ffmpeg

The maintainers' on-call engineer owns this. The daily trigger is the nightly e2e run: its
sandbox job's "Image vulnerabilities" step (tools/security/trivy-image.sh) fails as soon as a
HIGH or CRITICAL with a fix in the archive affects the worker image, and that failure is the
signal to bump. Every Monday, also read debian-security-announce (or
https://security-tracker.debian.org/tracker/source-package/ffmpeg) for trixie DSAs the gate
does not rate HIGH. For a DSA against ffmpeg, OpenSSL or glibc, bump within two working days;
otherwise bump at least monthly.

1. Pick a timestamp after the DSA's upload reached snapshot.debian.org: the `first_seen` of the
   `debian-security` archive in
   `https://snapshot.debian.org/mr/package/ffmpeg/<version>/binfiles/ffmpeg/<version>?fileinfo=1`
   (the version URL-encoded, `7%3A7.1.5-0%2Bdeb13u1`).
2. If Docker Hub has a newer `debian:trixie-slim` (a new 13.x point release), move the
   Dockerfile's `DEBIAN` digest to it. The base image's own packages (libc6, util-linux,
   perl-base, ncurses, libsystemd0 and the rest) are upgraded to the snapshot's versions at
   every build whatever the digest, but a current base keeps that upgrade small.
3. In a `debian:trixie-slim` container at the digest the Dockerfile names, with this checkout
   mounted, print the candidates at that timestamp:

   ```sh
   DEBIAN_SNAPSHOT=<timestamp> deploy/docker/apt-install-debian.sh --policy \
       ffmpeg ca-certificates openssl libcurl4t64 libpq5 libssl3t64 openssl-provider-legacy
   ```

4. Set `DEBIAN_SNAPSHOT` in `deploy/docker/Dockerfile` to the timestamp and every pinned version
   in the `worker` stage to its candidate. A build fails if a pin is not
   what the snapshot holds, so nothing drifts silently.
5. If ffmpeg's upstream version changed (the part before `-0+deb13u`, say 7.1.5 to 7.1.6; a
   `+deb13uN` patch alone does not need it), run in that container, with ffmpeg and strace
   installed, `tools/trace-ffmpeg-syscalls.sh` as root and as an ordinary user (setpriv, from
   util-linux, when given a uid; `runuser -u <user> --` otherwise) and
   `tools/ffmpeg-address-space.sh --full`. Extend `infra/ffmpeg/src/seccomp_filter.hpp` with
   any new call, each with its reason (docs/adr/0048); a command line that nears its address
   space limit is a finding for docs/adr/0074 before it is a bump. The trixie workflow runs both
   on every pull request that touches the Dockerfile, so its log shows them too.
6. Open the pull request, then dispatch the e2e workflow on its branch (Actions, e2e, "Run
   workflow"): pull requests do not run it. Its sandbox job builds the worker and live-packager
   images, runs the VOD flow through the worker, checks the release binaries' hardening and runs
   the Trivy gate over both. Once merged and published, operators deploy it as usual (step 4).

## 5. Verify

1. The deployments are up and the routes attached:

   ```sh
   kubectl -n "$NS" get deploy video-gateway video-worker chat     # 2/2, 1/1 and 3/3
   kubectl -n "$NS" logs deploy/video-gateway -c migrate           # "applied …" or "schema is up to date"
   for route in video-gateway chat livekit; do
     kubectl -n "$NS" get httproute "$route" \
       -o jsonpath='{.status.parents[*].conditions[?(@.type=="Accepted")].status}{"\n"}'   # True
   done
   kubectl -n "$NS" logs -l app.kubernetes.io/name=chat --prefix | grep '"listening"'
   ```

   Each chat pod's `listening` line names `"allocator":"jemalloc 5.3.0-..."` (docs/adr/0081)
   and its `node_address`, the pod's own IP and port 9201. A chat Deployment stuck below 3/3
   with `/readyz` answering 503 is usually a node that cannot publish its address or reach the
   database: its log says which.

   A route with `Accepted` missing or False and every request 404: check `GATEWAY_NAME` and
   `GATEWAY_NAMESPACE`, and that the Gateway's listener allows routes from `NAMESPACE`. A route is
   also not Accepted (`NoMatchingListenerHostname`) when `PUBLIC_HOSTNAME` falls within no
   listener hostname of the Gateway (a `*.example.com` listener does not match the apex
   `example.com`).
2. An end-to-end run with a real token from your identity provider:

   ```sh
   ULW_E2E_URL=https://<PUBLIC_HOSTNAME> ULW_E2E_TOKEN=<token> tests/cluster/vod_flow.py upload playback
   ```

   It uploads a 6 s clip through the route, waits for `ready`, then fetches the master and
   media playlists through the route and every segment from the store at the presigned URLs.
   Against a real URL the script runs only these two scenarios, only when named, and never runs
   kubectl.
3. A pod kill mid-upload, by hand: the scripted `pod-kill` refuses every cluster but the
   sandbox. While the upload scenario runs in a loop,

   ```sh
   while ULW_E2E_URL=https://<PUBLIC_HOSTNAME> ULW_E2E_TOKEN=<token> \
       tests/cluster/vod_flow.py upload; do :; done
   ```

   delete one gateway pod in another terminal, then the other once its replacement is Ready:

   ```sh
   kubectl -n "$NS" get pods -l app.kubernetes.io/name=video-gateway
   kubectl -n "$NS" delete pod <name>
   ```

   Every run must still end `ok`: a chunk cut off by the drain is resumed from `HEAD`'s offset.
   Stop the loop with Ctrl-C.
4. The client address the gateway sees. From a machine outside the cluster, note its public
   address (`curl -s https://ifconfig.me`), turn on debug logging, make a few requests through
   the route (Envoy spreads them over both replicas), and read both replicas' logs. They must
   show that address:

   ```sh
   kubectl -n "$NS" set env deploy/video-gateway ULW_LOG_LEVEL=debug
   kubectl -n "$NS" rollout status deploy/video-gateway
   for i in 1 2 3 4; do
     curl -s -o /dev/null https://<PUBLIC_HOSTNAME>/api/v1/videos/00000000-0000-7000-8000-000000000000
   done
   kubectl -n "$NS" logs -l app.kubernetes.io/name=video-gateway -c gateway --prefix \
     | grep '"forwarded client"' | tail -4
   kubectl -n "$NS" set env deploy/video-gateway ULW_LOG_LEVEL-
   ```

   `kubectl set env` changes the pod template, so each change rolls both pods (uploads in
   flight resume, as in step 3); under a GitOps tool, pause its sync for the check.

   An address inside `POD_CIDR`, or a node's, means Envoy's Service is not
   `externalTrafficPolicy: Local` or another proxy stands in front: fix that, or the hop count,
   before production.
5. Memory under load, in the sandbox only: `make e2e-load` (with `make e2e-up` run first)
   installs metrics-server, uploads through the route with 500 uploads in flight, samples
   `kubectl top pods --containers` every 10 s, and fails if a gateway or worker container goes
   over the limits in `base/video-gateway/deployment.yaml` and `base/video-worker/deployment.yaml`
   or restarts. The check refuses every cluster but the sandbox: a load generator must not be
   pointed at a shared cluster. On your cluster, record
   `kubectl -n "$NS" top pod -l app.kubernetes.io/name=video-gateway` at rest and after step 2,
   and compare it with the e2e workflow's report.
6. Chat through the route. The sandbox's `chat` scenario (`make e2e-test`) joins a stream's live
   chat from six sockets across the three nodes and checks one message reaches them all; on
   your cluster, check the upgrade with a real token, kept in a file so it stays off the command
   line (`Authorization: Bearer <token>` on one line):

   ```sh
   curl -si --http1.1 --max-time 3 -H @token.header -H 'Upgrade: websocket' \
     -H 'Connection: Upgrade' -H 'Sec-WebSocket-Version: 13' \
     -H "Sec-WebSocket-Key: $(openssl rand -base64 16)" https://<PUBLIC_HOSTNAME>/rt | head -1
   ```

   It must print `HTTP/1.1 101 Switching Protocols` (curl then times out holding the socket,
   which is expected); without the header line it is `401`. A `404` means the route is not
   attached (as above). From a page on one of `ALLOWED_ORIGINS` the cookie works too; from any
   other origin it is `403` (docs/integration/chat.md).

## 6. Rollback

A pinned environment's rollback is a change of its four image tags in `config.env` back to the
good build's `<sha>@sha256:<digest>`, from that build's publish run summary, and an apply. An
environment that follows `main` is pointed at the good build the same way: every publish is kept
under its full SHA, which never moves, so set each tag from `main` to `<good sha>` (a SHA on main
whose publish-images run succeeded), apply, and set it back to `main` once main is good again.
In a hurry, knowing that the next apply (or GitOps sync) of the overlay undoes it:

```sh
r=ghcr.io/jerome-joseph-1
kubectl -n "$NS" set image deployment/video-gateway migrate="$r/ulw-video-gateway:<good sha>" \
  gateway="$r/ulw-video-gateway:<good sha>"
kubectl -n "$NS" rollout status deployment/video-gateway
kubectl -n "$NS" set image deployment/video-worker worker="$r/ulw-video-worker:<good sha>"
kubectl -n "$NS" set image deployment/chat chat="$r/ulw-chat:<good sha>"
kubectl -n "$NS" set image cronjob/upload-reaper reaper="$r/ulw-video-gateway:<good sha>"
```

Packagers already running keep their build; start new streams with the good
`LIVE_PACKAGER_IMAGE_TAG` (step 9). A rollback across a change of chat's node-channel version is
a `Recreate` too (step 3).

Migrations only add (docs/adr/0031), so the older build runs against the newer schema; the
database is never rolled back.

To take the plane out entirely: delete the HTTPRoute first (uploads stop at the edge), then
scale both deployments to 0. Uploads in progress resume once it is back. Chat the same way: its
HTTPRoute, then `deployment/chat` to 0; clients reconnect and resume from their last seq once it
is back.

## 7. The realtime plane: STUNner and LiveKit

Media from browsers enters through STUNner, a TURN server run as a Gateway API implementation
beside Envoy Gateway (docs/adr/0013); LiveKit is the SFU behind it (docs/adr/0020). LiveKit hands
each client a TURN credential minted from a secret it shares with STUNner (docs/adr/0037). The
plane is in the base, so applying an overlay applies it; its pods wait for the secrets below.

### Check the cluster first

```sh
# v1.x, from Envoy Gateway:
kubectl get crd gateways.gateway.networking.k8s.io \
  -o jsonpath='{.metadata.annotations.gateway\.networking\.k8s\.io/bundle-version}'
kubectl get svc -A | grep -w "$TURN_PORT"     # nothing may hold the UDP port yet
```

STUNner's Gateway is a LoadBalancer Service on UDP `TURN_PORT` with
`externalTrafficPolicy: Local`, so TURN clients keep their own source address. Open that port
to the internet on your firewall, and nothing else: every call's media arrives on that one
port, and LiveKit's own UDP port (7882) stays inside the cluster. Two environments whose
LoadBalancer Services share one address (a single node's ServiceLB, say) each need their own
`TURN_PORT`, as the example overlays have (3478 and 3479; docs/adr/0084); with an address per
environment both can use 3478.

Whether your load balancer keeps the client's address is not tested anywhere before you deploy:
the sandbox has no load balancer and publishes a NodePort instead. The probe in "Verify" tells:
`binding.mapped` must be the probing machine's public address. If it is a node or pod address
instead, calls still work (media goes through the relay, not the reflexive address), but STUNner
sees every client as the node, so its logs and any per-client limit lose the client. The fix is
the path the sandbox proves, a NodePort under `externalTrafficPolicy: Local` with the node
forwarding the port to it, as annotations on the Gateway patched in your overlay
(deploy/stunner/kustomization.yaml shows them):

```sh
kubectl -n "$NS" annotate gateway stunner --overwrite \
  stunner.l7mp.io/service-type=NodePort 'stunner.l7mp.io/nodeport={"turn-udp": 31478}'
# on the node, persisted in its firewall configuration:
iptables -t nat -A PREROUTING -p udp --dport "$TURN_PORT" -j REDIRECT --to-ports 31478
```

Running stunnerd in the host's network, the other usual fix, is ruled out: pods may not use
`hostNetwork` (docs/adr/0013).

### Secrets

STUNner holds one shared secret for the whole cluster, and LiveKit in each environment signs
its clients' TURN credentials with that same secret (docs/adr/0037). So `TURN_SECRET` is one
value per cluster: a copy in an environment's `SFU_SECRET` that differs breaks every call of
that environment.

```sh
TURN_SECRET=$(openssl rand -base64 48 | tr -d '\n')     # once per cluster; keep it
# From a file descriptor, so the secret never appears in a process list. stunner-system exists
# once cluster/stunner/operator.yaml is applied (below).
kubectl -n stunner-system create secret generic stunner-secrets \
  --from-literal=type=ephemeral --from-file=secret=<(printf '%s' "$TURN_SECRET") \
  --dry-run=client -o yaml | kubectl apply -f -
```

Per environment, the realtime plane's Secret (`SFU_SECRET`, `sfu-secrets` by default):

| Key | What |
|---|---|
| `LIVEKIT_KEYS` | `"<api key>: <api secret>"`, the secret at least 32 characters; LiveKit checks tickets with it |
| `LIVEKIT_API_KEY`, `LIVEKIT_API_SECRET` | The same pair split in two, for egress and for chat's call handler |
| `LIVEKIT_CLIENT_URL` | `wss://<PUBLIC_HOSTNAME>`, where browsers reach LiveKit's `/rtc` route; every call ticket names it |
| `REDIS_PASSWORD` | 32+ random letters and digits; LiveKit's and egress's Redis |
| `TURN_HOST` | Where browsers reach STUNner: the LoadBalancer's public address, or a DNS name for it (TURN over UDP needs no certificate) |
| `TURN_SECRET` | The cluster's one TURN secret, as above |

```sh
LIVEKIT_KEYS="API$(openssl rand -hex 6): $(openssl rand -base64 36 | tr -d '\n')"
kubectl -n "$NS" create secret generic sfu-secrets \
  --from-literal=LIVEKIT_KEYS="$LIVEKIT_KEYS" \
  --from-literal=LIVEKIT_API_KEY="${LIVEKIT_KEYS%%: *}" \
  --from-literal=LIVEKIT_API_SECRET="${LIVEKIT_KEYS#*: }" \
  --from-literal=LIVEKIT_CLIENT_URL="wss://$PUBLIC_HOSTNAME" \
  --from-literal=REDIS_PASSWORD="$(openssl rand -hex 24)" \
  --from-literal=TURN_HOST="<public address or DNS name>" \
  --from-file=TURN_SECRET=<(printf '%s' "$TURN_SECRET") \
  --dry-run=client -o yaml | kubectl apply -f -
```

Generate a key pair per environment, never shared. Chat's call handler (docs/adr/0087) signs
tickets with the same pair LiveKit checks them with, and calls LiveKit's server API at
`http://livekit:7880` (a value in the chat Deployment). The chat Deployment reads the pair and
`LIVEKIT_CLIENT_URL` as optional: without them chat starts with calls off and answers every call
with `calls_disabled`. With calls on, the first ticket of a call rings the other member, for
45 s unless the chat container's environment sets `ULW_CALL_RING_TIMEOUT_MS` (1000 to 300000;
docs/adr/0091); the base leaves it unset, and an overlay that wants another ring patches it in.
A group chat's call holds 8 devices unless `ULW_CALL_GROUP_PARTICIPANTS` says otherwise (3 to 16;
docs/adr/0095 derives the default from one SFU pod's capacity, about four full calls of eight
per 2-core pod), set the same way. Group calls need migration 0014 (the room's media
generation), which the gateway's init container applies before chat's new pods matter; they
change no node-channel frame, so chat rolls out as usual, with group calls answered `unavailable`
or `not_callable` by an old pod until the rollout finishes.
All of these read the Secret only at start, so after a change restart
Redis first (LiveKit does not start without it); STUNner rereads its secret by itself:

```sh
kubectl -n "$NS" rollout restart deployment/livekit-redis
kubectl -n "$NS" rollout status deployment/livekit-redis
kubectl -n "$NS" rollout restart deployment/livekit deployment/livekit-egress deployment/chat
```

To rotate `TURN_SECRET`: change it in `stunner-secrets` and in every environment's
`SFU_SECRET`, then restart LiveKit in each. Calls in progress reconnect once with credentials
under the new secret.

### Install, once per cluster

STUNner's CRDs come from its chart at v1.2.1, checked against the hash `deploy/local/tools.sh`
pins:

```sh
curl -fsSLo stunner-crds.yaml https://raw.githubusercontent.com/l7mp/stunner-helm/08555494a2fdb53c0f8a0146cfa1c951dbb83f1b/helm/stunner/crds/stunner-crds.yaml
echo "720ab0c18e0e51b8cee18259685061e03cc0d3d01e90a0c0fc20c5144351b279  stunner-crds.yaml" | sha256sum -c
kubectl apply --server-side -f stunner-crds.yaml
kubectl apply -f cluster/stunner/operator.yaml
kubectl -n stunner-system rollout status deployment/stunner-gateway-operator-controller-manager
# stunner-secrets, as above
kubectl apply -f cluster/stunner/dataplane.yaml -f cluster/stunner/gatewayclass.yaml
```

Do not install the chart's own Gateway API CRDs: Envoy Gateway owns them, and a second copy at
another version would fight it. The environment's Gateway, LiveKit, Redis and egress come with
its overlay (step 4); all of them are pulled by digest. To run an environment without the plane,
delete `stunner`, `livekit`, `livekit-redis` and `livekit-egress` from its rendering with a
kustomize `$patch: delete` per resource; chat then answers calls with `calls_disabled`.

LiveKit is sized for about 126 concurrent 1:1 calls (one replica, 500m CPU and 256Mi requested,
2 CPU and 1Gi at most; docs/integration/calls.md, Capacity). STUNner's pods come from the
cluster's one `Dataplane`, so every environment's stunnerd is sized alike (500m, 128Mi). Redis
takes 50m and 64Mi. Egress takes one concurrent stream in the base (1 CPU and 512Mi requested,
3 CPU at most) and two in `overlays/production` (5 CPU at most, 768Mi requested, 1536Mi at
most): it admits a stream only while 2 of its cores are idle under its 80% ceiling, so raise the
limit by 2.5 cores per further concurrent stream, if the node has them.

### Verify

```sh
kubectl get gatewayclass stunner-gatewayclass                 # ACCEPTED True
kubectl -n "$NS" get gateway stunner                          # PROGRAMMED True, ADDRESS the public one
kubectl -n "$NS" get udproutes.stunner.l7mp.io livekit \
  -o jsonpath='{.status.parents[0].conditions[*].type}={.status.parents[0].conditions[*].status}'
kubectl -n "$NS" get deploy stunner livekit livekit-redis livekit-egress   # 1/1 each
kubectl -n "$NS" logs deploy/livekit-egress | head      # connected to Redis, no errors
curl -s -o /dev/null -w '%{http_code}\n' "https://$PUBLIC_HOSTNAME/rtc/validate"   # LiveKit's 401, not 404
```

Egress has not run in the sandbox cluster (its image is ~5 GB; the local call suite runs it
under compose), so its pod settings here (uid 10001 with an empty home directory, a read-only
root filesystem, the service started without the image's PulseAudio entrypoint) are first proven
on your cluster: a stream relayed per step 9 whose playlist appears is the check.

From a machine outside the cluster (a laptop on another network), with the TURN secret in the
environment so it never reaches the command line or the shell history:

```sh
TURN_SECRET=<read from your secret store> \
  tests/cluster/turn_probe.py <TURN_HOST> "$TURN_PORT" \
  --permit <livekit pod IP> --forbid <any other pod IP> --forbid 127.0.0.1 --forbid <node IP>
```

It only sends STUN and TURN requests. Expect `binding.mapped` to be the machine's public
address (compare `curl -s https://ifconfig.me`): anything else means the node masquerades
the traffic and `externalTrafficPolicy` is not taking effect. Expect `allocate` a success
with `integrity: true` and a relayed address inside the cluster, the `permit` peer a success,
the `forbid` peer an error, and `wrong_password` and `expired` errors (400 or 401). With two
environments on one cluster, forbid the other environment's LiveKit pod too: each relay reaches
only its own LiveKit.

Calls through chat (docs/integration/calls.md). Each chat pod's first log line names where
tickets send clients, and its metrics say calls are on:

```sh
kubectl -n "$NS" logs deploy/chat | grep -m1 '"msg":"listening"'   # "calls":"wss://...", not "off"
kubectl -n "$NS" port-forward deploy/chat 19101:9101 >/dev/null & sleep 2
curl -s localhost:19101/metrics | grep calls_enabled                  # calls_enabled 1
kill %1
```

Then, from a page of an allowed origin signed in as a member of a direct chat (or a native
client with its bearer token): `join` the room, send `{"type":"call","room":"<room>",
"device":"<uuid>"}` and expect a `ticket` whose `url` is `LIVEKIT_CLIENT_URL`;
`room.connect(url, token)` with `livekit-client` must succeed, and a second member's must show
the first as a participant. `call_errors_total` stays at 0 on every pod; a growing
`{source="sfu",kind="refused"}` means chat and LiveKit hold different key pairs.

### Rollback

Delete the UDPRoute to stop media at once (`kubectl -n "$NS" delete udproutes.stunner.l7mp.io
livekit`; STUNner then relays to nothing and calls stop), then take the plane out of the overlay
as in "Install" so the next apply does not bring it back. The operator removes the stunnerd
Deployment and Service when its Gateway goes. If nothing else uses STUNner, delete
`cluster/stunner/*.yaml` and the CRDs last.

## 8. Signing key rotation

An identity provider may rotate its signing key with no overlap: the new key is made and the old
one deactivated at once, and the old `kid` leaves the JWKS. The gateway and chat each cache the
key set and remember verified tokens for up to 15 minutes each, so left alone they accept tokens
under the old key for up to 15 minutes after a rotation (docs/integration/auth.md, Key
rotation). SIGHUP refetches the key set at once and, when that fetch succeeds, replaces the keys
and forgets every remembered token (docs/adr/0082).

Add ULW to your rotation procedure: once the rotation has committed, in each environment that
trusts the rotated key:

```sh
for pod in $(kubectl -n "$NS" get pods -l app.kubernetes.io/name=video-gateway -o name); do
  kubectl -n "$NS" exec "$pod" -c gateway -- sh -c 'kill -HUP 1'
done
for pod in $(kubectl -n "$NS" get pods -l app.kubernetes.io/name=chat -o name); do
  kubectl -n "$NS" exec "$pod" -c chat -- sh -c 'kill -HUP 1'
done
```

`gateway_server` and `chat_server` are PID 1 in their containers, and the images' `sh` has
`kill` built in. Then check that every pod took it: each logs `auth cache drop requested`
once, its `auth_cache_drops_total` on `/metrics` went up by one, and its
`auth_cache_drop_pending` is back to 0 (the fetch that completes the drop normally lands well
under a second later).

```sh
kubectl -n "$NS" logs -l app.kubernetes.io/name=video-gateway -c gateway --since=5m | grep 'auth cache drop requested'
kubectl -n "$NS" logs -l app.kubernetes.io/name=chat -c chat --since=5m | grep 'auth cache drop requested'
```

A pod that did not log it still accepts old tokens until its next refetch; signal it again, or
restart the deployment instead (`kubectl -n "$NS" rollout restart deployment/video-gateway`, or
`deployment/chat`), which also clears both caches but drains every connection and takes longer.
Chat sockets already open are not closed by the drop; each still ends with its own token
(docs/integration/auth.md).

If the JWKS cannot be reached, the SIGHUP drops nothing yet: `auth_cache_drop_pending` stays at
1 and tokens under the old key keep working until a fetch succeeds, which then completes the
drop. That is the right trade for a routine rotation. For a suspected key compromise during a
JWKS outage, use `kubectl -n "$NS" rollout restart deployment/video-gateway deployment/chat`
instead: new pods start with no keys and refuse every token (`503`) until a fetch succeeds,
which fails closed.

## 9. Live streams: the packager

A live stream reaches viewers as HLS that its packager writes to the bucket (docs/adr/0046),
from LiveKit's recorder (egress), which the stream service starts once the publisher's WHIP
POST has succeeded (docs/adr/0053). A packager is one process per stream, so on the cluster it
is one Job per stream, made from `live-packager/job.yaml` (docs/adr/0083). What it needs:

- **LiveKit egress and its Redis** (step 7). Egress's pods carry
  `app.kubernetes.io/name: livekit-egress`, the only pods the packager's NetworkPolicy admits,
  and LiveKit's configuration names the same Redis. A stream past egress's capacity is refused
  (`Unavailable` from the relay) until one ends.
- **The stream service**, which makes each stream's Job and Secret, calls the relay with the
  stream's passphrase, and records the stream's chat live (step 3). It does not exist yet; until
  it does, the steps below start a packager by hand.

The relay's packager address (the SFU adapter's `packager_srt`, ADR-0053) is
`srt://{stream}.live-packager.<NAMESPACE>.svc.cluster.local:9000`: `base/live-packager/service.yaml`
gives each packager pod that name.

### Secrets and the database role

The packager needs only a few rights on the database (docs/integration/operator-contract.md), so
it gets a role of its own rather than the gateway's. Once, as a superuser, after the gateway's
migrations have run:

```sql
CREATE ROLE ulw_live LOGIN;
\password ulw_live
\connect ulw
SET ROLE ulw;   -- the tables' owner grants on them
GRANT SELECT, INSERT ON live_recordings TO ulw_live;
GRANT SELECT (id), INSERT ON videos TO ulw_live;
GRANT INSERT ON jobs TO ulw_live;
GRANT USAGE ON SEQUENCE jobs_id_seq TO ulw_live;
```

Its Secret (`LIVE_PACKAGER_SECRET`, `live-packager-secrets` by default), with a store token of
its own (object read and write on the bucket):

```sh
kubectl -n "$NS" create secret generic live-packager-secrets \
  --from-literal=ULW_DATABASE_URL="$LIVE_DATABASE_URL" \
  --from-literal=ULW_S3_ACCESS_KEY_ID="$LIVE_STORE_KEY_ID" \
  --from-file=ULW_S3_SECRET_ACCESS_KEY=<(printf '%s' "$LIVE_STORE_SECRET") \
  --dry-run=client -o yaml | kubectl apply -f -
```

Nothing to restart: each packager reads the secret when its Job starts.

The bucket keeps a stream's segments under `live/<stream>/`, and the recording is read back from
them after the stream ends, so they need an expiry of days, not hours (operator-contract.md).
Add a rule beside step 3a's, merged into the same `lifecycle.json`; the number of days is yours:

```json
{
  "ID": "expire-live-segments",
  "Status": "Enabled",
  "Filter": { "Prefix": "live/" },
  "Expiration": { "Days": 7 }
}
```

### Start a stream's packager by hand

The stream id names the Job and the pod's DNS record, so it must be a DNS label here: lowercase
letters, digits and `-`, 1 to 63 (ADR-0053). The passphrase is the stream's own, 10 to 79
characters, and goes only into its Secret and the relay request. The template takes the
environment's values from its `config.env`:

```sh
set -a; . overlays/<env>/config.env; set +a
STREAM=launch-2026
OWNER=<the broadcaster's user id (the token's subject)>
kubectl -n "$NAMESPACE" create secret generic "live-packager-$STREAM" \
  --from-file=ULW_LIVE_SRT_PASSPHRASE=<(openssl rand -hex 24 | tr -d '\n')
ULW_STREAM_ID=$STREAM ULW_STREAM_OWNER=$OWNER envsubst '${NAMESPACE} ${LIVE_PACKAGER_IMAGE_TAG}
  ${IMAGE_PULL_POLICY} ${STORAGE} ${R2_ACCOUNT_ID} ${S3_ENDPOINT} ${BUCKET}
  ${LIVE_PACKAGER_SECRET} ${ULW_STREAM_ID} ${ULW_STREAM_OWNER}' \
  < live-packager/job.yaml | kubectl apply -f -
kubectl -n "$NAMESPACE" logs -f "job/$STREAM"
```

Name the variables to `envsubst` exactly as above: the template also holds `$(POD_IP)`, which is
Kubernetes' to expand, not the shell's. The log's first line names the stream, the storage and
`ingest=<pod IP>:9000`; from then on the packager waits for its one SRT caller. The gateway
serves the stream's playlist at `GET /api/v1/live/launch-2026/index.m3u8` once the first segment
is stored (docs/integration/live.md).

The stream ends when its caller goes; the packager then writes `EXT-X-ENDLIST`, queues the
recording as a video and logs `recording: queued as video <id>`, and the Job completes. To end
it from the cluster instead, send the packager SIGUSR1
(`kubectl -n "$NAMESPACE" exec "job/$STREAM" -c packager -- sh -c 'kill -USR1 1'`;
`live_packager` is PID 1 in its container). A SIGTERM (a node drain, a `kubectl delete pod`)
drains instead: the process exits 0, the Job counts as complete, and the stream is left to be
continued by a new packager for the same stream id, which whoever started the stream must start
(the template's `backoffLimit` restarts only failures). The Job and its pod are removed a day
after they finish; delete the stream's Secret with them:

```sh
kubectl -n "$NAMESPACE" delete secret "live-packager-$STREAM"
```

Every packager runs ffmpeg under the worker's sandbox, so it needs what step 1 checks for the
worker (user namespaces) and step 2's seccomp profile on the node it lands on.
