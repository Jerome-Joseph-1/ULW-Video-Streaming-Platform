# Deploying ULW on Askedin

For the person with access to Askedin's cluster. Nothing in this repository applies these files
to it; every command below is yours to run. Stage first, prod the same way a release later.

What ships:

| Path | What |
|---|---|
| `overlays/{stage,prod}/video-gateway/` | Deployment (2 replicas, migrations in its init container), Service, HTTPRoute + BackendTrafficPolicy, NetworkPolicy |
| `overlays/{stage,prod}/video-worker/` | Deployment (1 replica), NetworkPolicy; no Service, no route |
| `overlays/{stage,prod}/upload-reaper/` | CronJob (every 15 minutes) and NetworkPolicy; the gateway image's `ulw_reaper` (step 3a) |
| `seccomp/ulw-worker.json` | The worker's seccomp profile, installed on the node (step 2) |
| `overlays/{stage,prod}/chat/` | Deployment (3 replicas, docs/adr/0019), Service, HTTPRoute for its WebSocket (`/rt`), NetworkPolicy; secrets and Postgres settings in step 3 |
| `overlays/{stage,prod}/live-packager/` | The headless Service that names each stream's packager in DNS, and the NetworkPolicy every packager runs under (step 9) |
| `live-packager/job.yaml` | One stream's packager, a Job made from this template per stream; not in the overlays, so ArgoCD never applies it (step 9, docs/adr/0083) |
| `woodpecker.yml` | Builds and pushes the four images (video-gateway, video-worker, chat, live-packager), then `rollout restart`; never applies a manifest |
| `stunner/` | The STUNner gateway operator, its dataplane template, the GatewayClass and GatewayConfig: once per cluster (step 7) |
| `overlays/{stage,prod}/stunner/` | The TURN Gateway (UDP 3478 on stage, 3479 on prod) and the UDPRoute to LiveKit |
| `overlays/{stage,prod}/livekit/` | LiveKit (1 replica), Service, HTTPRoute for its signalling (`/rtc`) and WHIP (`/whip`), NetworkPolicy |
| `overlays/{stage,prod}/livekit-redis/` | Redis (1 replica, nothing persisted), ClusterIP Service, NetworkPolicy: LiveKit's bus to egress |
| `overlays/{stage,prod}/livekit-egress/` | LiveKit's recorder (1 replica), NetworkPolicy; no Service: it relays live streams to their packagers (step 9) |

The realtime plane is stage only until its phase is tagged there: STUNner, LiveKit and its
Redis with phase-4 (one-to-one calls), egress with phase-6 (live). Its prod overlays ship, but
apply each only after its phase tag, and after the steps only Askedin can take (a public UDP
port, a DNS name, prod's own keys): step 7, "Prod".
Live streams also need a stream service to start each stream's packager, which does not exist
yet: until it does, a packager is started by hand (step 9).

Open decisions, yours: whether this builds inside the Askedin monorepo or pushes from this
repository (the image names `git.askedin.com/askedin/askedin-monorepo/<svc>` assume the
monorepo), and stage's `JWT_ISSUER`. Askedin's `JWKS_URL` for each environment and prod's
`JWT_ISSUER` are set in the overlays (docs/integration/auth.md, Askedin); stage's issuer stays in
the secret until it is read from the live one (step 3). Askedin's key rotation needs a step on
their side that reaches ULW: step 8.

## 1. Check the cluster can run the worker

The worker gives each ffmpeg its own namespaces (docs/adr/0032), which needs pods in user
namespaces. On k8s-prod:

```sh
kubectl version                        # server v1.33 or later
uname -r                               # 6.3 or later (idmapped mounts on overlayfs)
k3s --version; runc --version          # containerd 2.x, runc 1.2 or later
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
  (the sandbox undoes it in deploy/local/e2e-up.sh); a K3s host normally has no such mount.

One difference the sandbox cannot show: containerd inside a kind node never applies AppArmor,
while K3s on a host where `cat /sys/module/apparmor/parameters/enabled` prints `Y` gives every
pod its default AppArmor profile, which denies `mount`. On such a node watch the worker's first
start; a `mount /proc` error in its log means the worker needs its own AppArmor profile
(`securityContext.appArmorProfile`) before it can run.

The gateway's and chat's NetworkPolicies admit only Envoy's data plane, found by labels.
Confirm them, or edit `overlays/*/video-gateway/networkpolicy.yaml` and
`overlays/*/chat/networkpolicy.yaml` before the first apply:

```sh
kubectl get pods -A -l app.kubernetes.io/component=proxy,app.kubernetes.io/managed-by=envoy-gateway \
  -o custom-columns=NS:.metadata.namespace,NAME:.metadata.name   # expect namespace envoy-gateway-system
```

It also admits Prometheus from namespace `monitoring` to scrape `/metrics`. If Prometheus runs
elsewhere, change the second `from` in the same files:

```sh
kubectl get pods -A -l app.kubernetes.io/name=prometheus -o custom-columns=NS:.metadata.namespace
```

Both the gateway's and the worker's NetworkPolicies also limit what they reach out to: cluster
DNS in `kube-system`, and TCP 5432 (Postgres) and 443 (R2; for the gateway also Askedin's JWKS).
The worker's 5432 and 443 exclude the pod network, `10.42.0.0/16` (K3s's default; change it in
`overlays/*/video-worker/networkpolicy.yaml` if the cluster's differs). The gateway's 443 does
not, in case `JWKS_URL` names an in-cluster Service; a policy matches the pod's own port, so if
that Service's pods listen on a port other than 443, add it to the gateway's egress rule, or the
gateway can fetch no keys (`auth_failures` rise with 503s). Check, before the first apply:

```sh
kubectl get pods -n kube-system -l k8s-app=kube-dns -o name        # cluster DNS is in kube-system
kubectl get nodes -o jsonpath='{.items[*].spec.podCIDR}'           # each a /24 in 10.42.0.0/16
```

After it, the gateway's log shows no failed key fetch and its `/readyz` stays 200; a worker
that cannot reach the bucket fails its first job with a store error.

The gateway limits each client address (20 connections, or 20 requests in flight before they
are authenticated, and 10 new connections a second) and each user (300 requests a minute, 100
GiB of uploads a day), per replica. Behind Envoy every connection comes from Envoy's pods, so
the gateway takes the client address from `X-Forwarded-For`, from peers in `ULW_TRUSTED_PROXIES`
only, and only the entry the `ULW_TRUSTED_PROXY_HOPS` proxies in front appended: with Envoy
alone, the last one. The overlays set K3s's default pod network, `10.42.0.0/16`, and one hop.
Confirm Envoy's pods are in that block, and that Envoy sees clients' own addresses rather than
a node's: its Service must have `externalTrafficPolicy: Local`, or kube-proxy and ServiceLB
rewrite every client to a node address before Envoy appends it.

```sh
kubectl get pods -n envoy-gateway-system -l app.kubernetes.io/component=proxy \
  -o custom-columns=NAME:.metadata.name,IP:.status.podIP
kubectl get nodes -o jsonpath='{.items[*].spec.podCIDR}'    # a /24 inside the cluster's block
kubectl get svc -n envoy-gateway-system -l app.kubernetes.io/component=proxy \
  -o custom-columns=NAME:.metadata.name,POLICY:.spec.externalTrafficPolicy   # Local
```

If the pods are elsewhere, set `ULW_TRUSTED_PROXIES` in both
`overlays/*/video-gateway/deployment.yaml` and both `overlays/*/chat/deployment.yaml` to the
block that holds them before the first apply.
With the wrong block every client counts as Envoy: the gateway resets Envoy's connections past
20 and `connections_rejected_total{reason="ip_connections"}` climbs. With a proxy more or fewer
in front than `ULW_TRUSTED_PROXY_HOPS` says, clients are counted as the wrong address; step 5.4
checks which one the gateway sees.

`chat_server` holds each client address to a share of its connections too (20 open sockets, 80
from one IPv6 /48, 10 new ones a second, and 16 sockets per user; docs/adr/0076), and reads the
same `ULW_TRUSTED_PROXIES` and `ULW_TRUSTED_PROXY_HOPS`; `overlays/*/chat/deployment.yaml` sets
both exactly as the gateway's, so change them in both places together. Without them every
WebSocket arrives from
Envoy's pods, so all clients together share Envoy's 20 sockets per address: the 21st client
through an Envoy pod is reset at accept, and
`connections_rejected_total{reason="ip_connections"}` on the chat node climbs while its
`connections_current` is nowhere near 1280, the node's fixed cap. With the proxies set, the chat
node tells clients apart by `X-Forwarded-For` at their upgrade requests, and it refuses
`ULW_TRUSTED_PROXY_HOPS` without `ULW_TRUSTED_PROXIES` and warns of a block wider than /8 (IPv6
/32), as the gateway does.

The NetworkPolicy admits Envoy's pods and every pod in the `monitoring` namespace, both from
inside the trusted block. A monitoring pod can therefore send any `X-Forwarded-For` it likes; all
that buys it is choosing which address its own unauthenticated requests are counted against,
and it holds no token to do more.

The other limits are environment variables in the same files (`ULW_MAX_CONNECTIONS_PER_IP`,
`ULW_NEW_CONNECTIONS_PER_IP_PER_SECOND`, `ULW_REQUESTS_PER_USER_PER_MINUTE`,
`ULW_UPLOAD_BYTES_PER_USER_PER_DAY`), listed with their ranges in
`docs/integration/operations-contract.md`; the defaults and their derivations are in
docs/adr/0052. Each applies per replica, so with two replicas a user may reach twice a per-user
limit. The byte quota is best effort: it lives in each replica's memory, is forgotten on a
restart, and a user unseen while 16,384 others were active starts over.

The pods start as user 10001 and have nothing to drop. A process started as root (a
hand-started binary, a supervisor that stays root) must be given `ULW_RUN_AS_USER`, which it
becomes after binding its port and before it serves, or it refuses to start with exit 2;
`ULW_ALLOW_ROOT=1` lets it stay root, for development only. After the drop it reads the TLS
certificate and key (at start, and again on every SIGHUP), so with `ULW_TRANSPORT=tls` both
files must be readable by that user.

Check the node has room. Both environments run on k8s-prod's 8 vCPU / 24 GB, and the new
requests are, per environment, 2 x 500m CPU and 2 x 600Mi for the gateways plus the worker's
2Gi, 1 CPU / 10Gi of scratch (stage) or 2 CPU / 30Gi (prod) for the worker, and 3 x 250m CPU and
3 x 832Mi for chat: 6.5 CPU, 11.3Gi of memory and 40Gi of ephemeral storage for both. Each live
stream adds a packager's 100m CPU, 256Mi of memory and 256Mi of ephemeral storage while it runs
(step 9). Compare with what is already allocated:

```sh
kubectl describe nodes | sed -n '/Allocated resources/,/Events/p'
df -h /var/lib/kubelet                 # the scratch emptyDirs live here
```

If the node cannot take it, lower the prod worker's CPU request, or chat's (its quarter core per
pod is not measured; `overlays/*/chat/deployment.yaml` says so), before the first prod apply.
The gateways' memory requests are the budget of docs/adr/0027 and chat's the worst case of
docs/adr/0036; neither should move. A single node also means three chat pods share one failure
domain: ADR-0019's point about a drain taking two of them at once becomes all three, and no
PodDisruptionBudget is shipped, since on one node it would only stop the drain.

Check that the JWKS host speaks TLS 1.3. The gateway, the worker and chat refuse any https
server that does not (docs/integration/operations-contract.md), so a JWKS host stuck on TLS
1.2 leaves every request `503` until it is fixed. From any machine:

```sh
openssl s_client -tls1_3 -connect auth.askedin.com:443 -servername auth.askedin.com </dev/null \
    2>&1 | grep -E '^ *Protocol *:|New, TLSv1.3'
```

It must print TLSv1.3; a handshake failure means the host has to enable TLS 1.3 before this
deploys. Use the host of the real `JWKS_URL` if it is not auth.askedin.com. R2 needs no check:
Cloudflare serves TLS 1.3.

## 2. Install the worker's seccomp profile on k8s-prod

Once per node, and again whenever `seccomp/ulw-worker.json` changes:

```sh
sudo install -D -m 0644 seccomp/ulw-worker.json /var/lib/kubelet/seccomp/profiles/ulw-worker.json
```

Without it the worker pod stays in `CreateContainerError`, and so does every live packager
(step 9), which runs ffmpeg under the same sandbox and profile.

## 3. Secrets

New keys for `.env.stage` and `.env.prod` (names only). The R2 bucket needs the lifecycle rule
of step 3a; the gateway expires its uploads after 6 days.

```
VIDEO_DATABASE_URL                    postgresql://… for the ulw database; the role needs DDL (docs/adr/0031)
VIDEO_R2_ACCOUNT_ID
VIDEO_R2_BUCKET
VIDEO_GATEWAY_R2_ACCESS_KEY_ID        R2 token for the gateway: object read and write on the bucket
VIDEO_GATEWAY_R2_SECRET_ACCESS_KEY
VIDEO_WORKER_R2_ACCESS_KEY_ID         R2 token for the worker: object read and write on the bucket
VIDEO_WORKER_R2_SECRET_ACCESS_KEY
VIDEO_JWT_ISSUER                      .env.stage only: the iss Askedin's stage auth-service puts in its tokens
VIDEO_CHAT_NODE_SECRET                32+ random bytes, base64 (openssl rand -base64 48); docs/adr/0035
```

`JWKS_URL` is no longer a secret: the overlays set Askedin's for each environment, and prod's
overlay sets `JWT_ISSUER` (`https://auth.askedin.com`) too, so `.env.prod` needs no
`VIDEO_JWT_ISSUER`. Stage's is not confirmed: Askedin's deployment template says
`https://auth-stage.askedin.com/auth`, but `iss` is compared byte for byte and a wrong value
refuses every token, so read the live one first and put exactly that in `.env.stage`:

```sh
kubectl -n apps-stage get secret auth-service-secrets -o jsonpath='{.data.ISSUER}' | base64 -d
```

Once it is confirmed it can move into the stage overlay as prod's has (docs/integration/auth.md,
Askedin, then says so too).

Lines for `scripts/create-k8s-secrets.sh`, after it has sourced the env file and set `$NS`
(`apps` or `apps-stage`) and `$ASKEDIN_ENV`:

```sh
kubectl -n "$NS" create secret generic video-gateway-secrets \
  --from-literal=ASKEDIN_ENV="$ASKEDIN_ENV" \
  --from-literal=ULW_DATABASE_URL="$VIDEO_DATABASE_URL" \
  --from-literal=ULW_R2_ACCOUNT_ID="$VIDEO_R2_ACCOUNT_ID" \
  --from-literal=ULW_BUCKET="$VIDEO_R2_BUCKET" \
  --from-literal=ULW_S3_ACCESS_KEY_ID="$VIDEO_GATEWAY_R2_ACCESS_KEY_ID" \
  --from-literal=ULW_S3_SECRET_ACCESS_KEY="$VIDEO_GATEWAY_R2_SECRET_ACCESS_KEY" \
  --from-literal=JWT_ISSUER="${VIDEO_JWT_ISSUER:-}" \
  --dry-run=client -o yaml | kubectl apply -f -
kubectl -n "$NS" create secret generic video-worker-secrets \
  --from-literal=ASKEDIN_ENV="$ASKEDIN_ENV" \
  --from-literal=ULW_DATABASE_URL="$VIDEO_DATABASE_URL" \
  --from-literal=ULW_R2_ACCOUNT_ID="$VIDEO_R2_ACCOUNT_ID" \
  --from-literal=ULW_BUCKET="$VIDEO_R2_BUCKET" \
  --from-literal=ULW_S3_ACCESS_KEY_ID="$VIDEO_WORKER_R2_ACCESS_KEY_ID" \
  --from-literal=ULW_S3_SECRET_ACCESS_KEY="$VIDEO_WORKER_R2_SECRET_ACCESS_KEY" \
  --dry-run=client -o yaml | kubectl apply -f -
kubectl -n "$NS" create secret generic chat-secrets \
  --from-literal=ASKEDIN_ENV="$ASKEDIN_ENV" \
  --from-literal=ULW_DATABASE_URL="$VIDEO_DATABASE_URL" \
  --from-literal=ULW_NODE_SECRET="$VIDEO_CHAT_NODE_SECRET" \
  --from-literal=JWT_ISSUER="${VIDEO_JWT_ISSUER:-}" \
  --dry-run=client -o yaml | kubectl apply -f -
```

Chat uses the gateway's database and role: its tables come from the same migrations, which the
gateway's init container applies (docs/adr/0031).

Add the three deployments to the script's `kubectl rollout restart` list, so a secret change
reaches them:

```sh
kubectl -n "$NS" rollout restart deployment/video-gateway deployment/video-worker deployment/chat
```

The chat nodes refuse each other unless they hold the same `ULW_NODE_SECRET`, so a new value
must reach every pod: the restart above rolls them one at a time, and while old and new pods
overlap, rooms owned across the two are unreachable from the other side and their joins and
sends fail as `unavailable` (clients retry). To rotate it without that, scale chat to 0 and back
to 3 instead of the restart.

The worker gets no JWT settings at all. Prod's gateway and chat do not read `JWT_ISSUER` from
the secret, so the empty value `.env.prod` leaves there is never used.

The role and database on k8s-prod's Postgres, once, as a superuser (psql prompts for the
password with `\password`; it never goes on a command line):

```sql
CREATE ROLE ulw_stage LOGIN;           -- ulw_prod for prod
\password ulw_stage
CREATE DATABASE ulw_stage OWNER ulw_stage;
```

The role owns its database, which gives the migrations their DDL rights (docs/adr/0031).
`VIDEO_DATABASE_URL` is then `postgresql://ulw_stage:<password>@<host>:5432/ulw_stage`, with the
password percent-encoded.

Chat message bodies travel as bound parameters, which the server writes to its log whenever it
logs a statement with its parameters or an error in one (docs/adr/0054). Keep them out, on the
same database, as the same superuser, whatever statement logging is on now or later:

```sql
ALTER DATABASE ulw_stage SET log_parameter_max_length = 0;
ALTER DATABASE ulw_stage SET log_parameter_max_length_on_error = 0;
-- Only where auto_explain is loaded:
ALTER DATABASE ulw_stage SET auto_explain.log_parameter_max_length = 0;
```

`SHOW log_parameter_max_length;` and `SHOW log_parameter_max_length_on_error;` in a new session
as `ulw_stage` then print `0`.

Chat rooms other than a stream's live chat admit only their listed members (docs/adr/0054).
Until the product manages the lists, they are rows in `chat_members`, set as the service's role.
Record the room as closed in the same transaction, before its first member, as the service's own
statement does, so that it can never be recorded live while it lists anyone:

```sql
BEGIN;
INSERT INTO chat_rooms (room_id, kind) VALUES ('<room uuid>', 'group_chat')
ON CONFLICT (room_id) DO NOTHING;
INSERT INTO chat_members (room_id, user_id) VALUES ('<room uuid>', '<user sub>')
ON CONFLICT (room_id, user_id) DO NOTHING;
COMMIT;

DELETE FROM chat_members WHERE room_id = '<room uuid>' AND user_id = '<user sub>';
```

A member removed this way is cut off at once on every chat node, however the row goes (a DELETE,
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
(`chat_rooms_live_is_a_stream`), since every chat node tells a live chat by its id alone. Until the product calls `IMessageStore::record_live` when a stream goes
on air, open a stream's chat before its viewers arrive, as the service's role, with the stream's
name for `<stream>`:

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
peer of another version. A release that changes the version (M19 moves it from 2 to 3) splits a
rolling update in two: until the last old pod is gone, old and new nodes cannot reach each
other, rooms owned across the split are unreachable from the other side, and their joins and
sends fail as `unavailable` (clients retry them). Roll such a release out with the chat
Deployment's strategy set to `Recreate` (`spec.strategy: {type: Recreate}`), which stops every
old pod before starting the new ones: a short full outage instead of a split one. Releases
that keep the version roll as usual.

### 3a. Lifecycle rule and upload reaper

The bucket aborts incomplete multipart uploads under `videos/` after 7 days, a day past the
gateway's 6-day upload lifetime. This is the backstop; the reaper below normally gets there
within minutes of an upload expiring (docs/adr/0049). Once per bucket, with an R2 token that
has admin read and write on the bucket (neither the gateway's nor the worker's), and the AWS
CLI:

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
R2="https://$VIDEO_R2_ACCOUNT_ID.r2.cloudflarestorage.com"
aws s3api get-bucket-lifecycle-configuration --endpoint-url "$R2" --bucket "$VIDEO_R2_BUCKET"
aws s3api put-bucket-lifecycle-configuration --endpoint-url "$R2" --bucket "$VIDEO_R2_BUCKET" \
  --lifecycle-configuration file://lifecycle.json
aws s3api get-bucket-lifecycle-configuration --endpoint-url "$R2" --bucket "$VIDEO_R2_BUCKET"
```

The put replaces the bucket's whole lifecycle configuration: if the first get shows rules, merge
them into `lifecycle.json`. The same rule is in the dashboard under the bucket's Settings, Object
lifecycle rules, as "Abort incomplete multipart uploads" with prefix `videos/` and 7 days.

The reaper is `overlays/{stage,prod}/upload-reaper/`: a CronJob running the gateway image's
`ulw_reaper` every 15 minutes with the gateway's secret (the same `:development` / `:master`
image and `imagePullPolicy: Always` as the gateway, so the reaper's SQL and lock key match the
build the gateways run), and a NetworkPolicy that lets it reach cluster DNS, Postgres (5432) and
the store (443) and nothing else. It aborts uploads past their `expires_at`, fails their videos
with "upload expired", releases their storage sessions, removes any object a finished commit
left at their key, and aborts sessions older than the uploads' lifetime that no upload owns
(docs/adr/0049). It also forgets direct and group chat rooms that a refused join recorded more
than a day ago and nothing used since (no members, never on the room plane), however old; a
stream's live chat is never forgotten. It looks at 10,000 rooms a pass at most, from where the
last pass stopped, and starts over from the oldest once it reaches the cutoff
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

The NetworkPolicy allows ports, not addresses, because Postgres runs on the node's host and R2
is on the internet. If the host's address is stable, add it as an `ipBlock` to the 5432 rule.

## 4. Pipeline and first deploy

1. Copy `overlays/stage/*` and `overlays/prod/*` into the monorepo's overlay tree, and
   `woodpecker.yml` into its pipeline directory. Give Woodpecker the secrets `registry_user`,
   `registry_password` and `kubeconfig`; the kubeconfig's user needs only `get`, `list`, `watch`
   and `patch` on deployments and `get`, `list`, `watch` on replicasets, in `apps` and
   `apps-stage` (`rollout restart` and `rollout status`). The image steps run the buildx plugin,
   which Woodpecker 3 runs privileged only when the server lists it: add the exact reference
   from `woodpecker.yml` (tag and digest) to the server's `WOODPECKER_PLUGINS_PRIVILEGED`.
2. Push to `development`. Woodpecker pushes `video-gateway:development`,
   `video-worker:development`, `chat:development` and `live-packager:development`; the restart
   step fails on the first run because the deployments do not exist yet. That is expected.
3. Bootstrap stage once:

   ```sh
   scripts/apply-k8s-overlay.sh stage --with-deployments
   ```

   `--with-deployments` applies every service's `deployment.yaml` in the stage overlay, not
   just these three. Any service whose live image or replica count was changed outside git (a
   `kubectl set image`, a manual scale) is reset to what git says. Check first with
   `kubectl -n apps-stage diff -f <each other service's deployment.yaml>`, and run it when a
   reset would not hurt.
4. From then on ArgoCD syncs the manifests and Woodpecker only restarts.

The route serves `/api/v1/uploads` and `/api/v1/videos` on every hostname of
`askedin-gateway`. If the video plane gets a hostname of its own, add `hostnames:` to both
`httproute.yaml` files.

### 4a. Deploying by digest

The overlays name the images by branch tag (`:development`, `:master`) with
`imagePullPolicy: Always`. A tag is mutable: whoever can push to the registry can change what
the next restart, reschedule or node drain runs, and two gateway pods started a minute apart can
run different builds. LiveKit, which is not built here, is already pinned by digest. Ours cannot
be written into the manifests at commit time, because the digest only exists once the pipeline
has built that very commit. So the pipeline has to write it, and ArgoCD, which owns the
manifests, then deploys exactly that digest. Nothing in `woodpecker.yml` does this yet: it needs
two things only you can set up, a Woodpecker secret allowed to push to the monorepo branch
ArgoCD follows, and ArgoCD rendering the overlay with kustomize. Once both exist:

1. Put a `kustomization.yaml` in each of `overlays/{stage,prod}/video-gateway`, `video-worker`,
   `upload-reaper` and `chat`, listing that directory's manifests under `resources:` and the
   image under `images:`. ArgoCD renders a directory with a `kustomization.yaml` through
   kustomize on its own. Change `imagePullPolicy: Always` to `IfNotPresent`: a digest never
   changes, so there is nothing to pull again.
2. Add a step to `woodpecker.yml` after the image steps, in place of `rollout-restart`: resolve
   the digest of the tag just pushed under the commit's SHA (not the branch tag, which a
   concurrent build may have moved), write it with kustomize, and commit it. With `crane` and
   `kustomize` in an image pinned by tag and digest like every other step:

   ```sh
   overlays=<path to overlays/ in the monorepo>   # fill in: the directory holding stage and prod
   case "$CI_COMMIT_BRANCH" in
     master) env=prod ;;
     development) env=stage ;;
   esac
   gw=git.askedin.com/askedin/askedin-monorepo/video-gateway
   wk=git.askedin.com/askedin/askedin-monorepo/video-worker
   ch=git.askedin.com/askedin/askedin-monorepo/chat
   gw_digest=$(crane digest "$gw:$CI_COMMIT_SHA")
   wk_digest=$(crane digest "$wk:$CI_COMMIT_SHA")
   ch_digest=$(crane digest "$ch:$CI_COMMIT_SHA")
   git config user.name "<pipeline commit name>"      # fill in
   git config user.email "<pipeline commit email>"    # fill in
   # The monorepo's clone URL, assumed from the registry path: confirm it. The token is read
   # from the environment on each call, never written to .git/config.
   git remote add deploy https://git.askedin.com/askedin/askedin-monorepo.git
   git config credential.helper \
     '!f() { echo "username=<push user>"; echo "password=$DEPLOY_PUSH_TOKEN"; }; f'
   pin() {
     (cd "$overlays/$env/video-gateway" && kustomize edit set image "$gw=$gw@$gw_digest")
     (cd "$overlays/$env/upload-reaper" && kustomize edit set image "$gw=$gw@$gw_digest")
     (cd "$overlays/$env/video-worker" && kustomize edit set image "$wk=$wk@$wk_digest")
     (cd "$overlays/$env/chat" && kustomize edit set image "$ch=$ch@$ch_digest")
     # Already pinned (a rerun, or the branch holds this digest): nothing to commit.
     git diff --quiet || git commit -qam "deploy: video images $CI_COMMIT_SHA [skip ci]"
   }
   pin
   for attempt in 1 2 3; do
     git push deploy "HEAD:$CI_COMMIT_BRANCH" && exit 0
     # Non-fast-forward: start again from the branch as it is now, not from a rebase.
     git fetch deploy "$CI_COMMIT_BRANCH" && git checkout -q FETCH_HEAD || exit 1
     # A code commit after this one builds and pins its own digest; leave it to that build.
     newer=$(git rev-list -1 --fixed-strings --invert-grep --grep='[skip ci]' \
       "$CI_COMMIT_SHA..HEAD")
     [ -z "$newer" ] || exit 0
     pin
   done
   exit 1
   ```

   Fill in `overlays` for the monorepo's layout, the commit identity and push user, and confirm
   the remote URL. The token comes from a Woodpecker secret, named in the step and never written
   into the file:

   ```yaml
   environment:
     DEPLOY_PUSH_TOKEN:
       from_secret: deploy_push_token
   ```

   Woodpecker checks out the commit as a detached HEAD, so the push names the branch,
   `HEAD:$CI_COMMIT_BRANCH`; a bare `git push` has no branch to push. The commit carries
   `[skip ci]`, so it does not build again; ArgoCD sees the new digest and rolls the deployments
   itself, the gateway's init container migrating first as today.

   When the push is refused because the branch moved, a rebase would always conflict if the
   commit that landed is another build's digest commit: both edit the same `digest:` lines from
   the same base. So the step fetches the branch, and if a code commit (one whose message lacks
   `[skip ci]`) has landed after `$CI_COMMIT_SHA`, it stops without pinning, because that newer
   build pins itself. Otherwise only digest commits landed, from builds of this commit or older
   ones, and it redoes its four edits on the fetched tree, commits and pushes, up to three
   times. The edits are regenerated rather than replayed, so a shallow clone (Woodpecker's
   default) is enough: the fetch brings the commits after the clone's, and the check reads only
   the range after `$CI_COMMIT_SHA`, which the clone has. The rule holds only while every commit
   on the branch runs this pipeline: if the monorepo's pipeline filters on paths (`when: path:`),
   a commit outside them never pins, so limit the check to the same paths
   (`git rev-list ... "$CI_COMMIT_SHA..HEAD" -- <the video paths>`). The `exit`s end the step,
   so the snippet is the step's last command.
   Drop the restricted kubeconfig then: the pipeline no longer touches the cluster.
   The live packager is not pinned this way: its Job template is filled in per stream, so
   whatever starts a stream names the image, and should name it by the digest of the build it
   means (`crane digest .../live-packager:<sha>`) rather than by the branch tag (step 9).
3. Rollback (section 6) becomes one of:
   - `kustomize edit set image` to the digest of `<good sha>`, committed with `[skip ci]`;
   - a revert of the digest commit, whose message must also carry `[skip ci]`
     (`git revert --no-edit <commit>`, then `git commit --amend` to add it). Without it the
     revert builds the branch's HEAD, the bad code, and the new digest step pins it again;
   - a revert of the bad code itself, which builds and pins a good image the normal way.

   Any of these instead of a retag and a restart. Before a rollback by digest, cancel or wait
   out any pipeline still running on the branch: one that finishes afterwards sees the rollback
   as another build's digest commit and pins its own image over it.

Until then, what a pod runs is at least recorded. This prints each pod's resolved digest, which
must match `crane digest <image>:<sha>` of the commit you meant to deploy:

```sh
kubectl -n apps-stage get pods -o \
  jsonpath='{range .items[*]}{.metadata.name}{" "}{.status.containerStatuses[*].imageID}{"\n"}{end}'
```

### Image builds and the worker's ffmpeg

The images come from two distributions (docs/adr/0074): `video-gateway` and `chat` are Ubuntu
24.04 with their packages from snapshot.ubuntu.com (chat adds the jemalloc it links,
docs/adr/0081), `video-worker` and `live-packager` are Debian 13 (trixie) with their packages,
ffmpeg among them, from snapshot.debian.org, one stage shared by both; every image's binaries
are built on Ubuntu, since only Ubuntu's glibc lets them carry the CET marks the hardening
check requires. On the Debian images those marks are a static property only: Debian's own
`libc.so.6` is unmarked, so the kernel does not enable a shadow stack for their processes. And
because their binaries run on trixie's libraries, the Ubuntu release they are built on must not
have a newer glibc or libstdc++ than trixie; the trixie workflow fails if they stop loading
there. Woodpecker's builder needs to reach both snapshot services over https, deb.debian.org
over http for the Debian stage's bootstrap of ca-certificates (signed and checked for
freshness), and Docker Hub for both base images. A builder behind a TLS-inspecting proxy passes
its CA as the build secret `ca-bundle`. Each image is one `target:` of `deploy/docker/Dockerfile`
(`gateway`, `worker`, `chat`, `live-packager`); a bump of the worker's ffmpeg below reaches the
live packager's image too.

#### Updating the worker's ffmpeg

The platform's on-call engineer owns this. The daily trigger is the nightly e2e run: its
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
   the Trivy gate over both. Deploy
   as usual (step 4).

## 5. Verify on stage (M14)

1. ArgoCD shows the stage application Synced and Healthy; then:

   ```sh
   kubectl -n apps-stage get deploy video-gateway video-worker chat # 2/2, 1/1 and 3/3
   kubectl -n apps-stage logs deploy/video-gateway -c migrate        # "applied …" or "schema is up to date"
   for route in video-gateway chat; do
     kubectl -n apps-stage get httproute "$route" \
       -o jsonpath='{.status.parents[*].conditions[?(@.type=="Accepted")].status}{"\n"}'   # True
   done
   kubectl -n apps-stage logs -l app.kubernetes.io/name=chat --prefix | grep '"listening"'
   ```

   Each chat pod's `listening` line names `"allocator":"jemalloc 5.3.0-..."` (docs/adr/0081)
   and its `node_address`, the pod's own IP and port 9201. A chat Deployment stuck below 3/3
   with `/readyz` answering 503 is usually a node that cannot publish its address or reach the
   database: its log says which.

   A route with `Accepted` missing or False is the stage 404 trap: check `parentRefs` names
   namespace `apps`.
2. An end-to-end run with a real stage token (from a browser session's `auth_token_stage`
   cookie):

   ```sh
   ULW_E2E_URL=https://<stage host> ULW_E2E_TOKEN=<token> tests/cluster/vod_flow.py upload playback
   ```

   It uploads a 6 s clip through the route, waits for `ready`, then fetches the master and
   media playlists through the route and every segment from R2 at the presigned URLs. Against a
   real URL the script runs only these two scenarios, only when named, and never runs kubectl.
3. A pod kill mid-upload, by hand: the scripted `pod-kill` refuses every cluster but the
   sandbox. While the upload scenario runs in a loop,

   ```sh
   while ULW_E2E_URL=https://<stage host> ULW_E2E_TOKEN=<token> \
       tests/cluster/vod_flow.py upload; do :; done
   ```

   delete one gateway pod in another terminal, then the other once its replacement is Ready:

   ```sh
   kubectl -n apps-stage get pods -l app.kubernetes.io/name=video-gateway
   kubectl -n apps-stage delete pod <name>
   ```

   Every run must still end `ok`: a chunk cut off by the drain is resumed from `HEAD`'s offset.
   Stop the loop with Ctrl-C.
4. The client address the gateway sees. From a machine outside the cluster, note its public
   address (`curl -s https://ifconfig.me`), turn on debug logging, make a few requests through
   the route (Envoy spreads them over both replicas), and read both replicas' logs. They must
   show that address:

   ```sh
   kubectl -n apps-stage set env deploy/video-gateway ULW_LOG_LEVEL=debug
   kubectl -n apps-stage rollout status deploy/video-gateway
   for i in 1 2 3 4; do
     curl -s -o /dev/null https://<stage host>/api/v1/videos/00000000-0000-7000-8000-000000000000
   done
   kubectl -n apps-stage logs -l app.kubernetes.io/name=video-gateway -c gateway --prefix \
     | grep '"forwarded client"' | tail -4
   kubectl -n apps-stage set env deploy/video-gateway ULW_LOG_LEVEL-
   ```

   `kubectl set env` changes the pod template, so each change rolls both pods (uploads in
   flight resume, as in step 3), and ArgoCD shows the Deployment OutOfSync until the second
   one undoes the first; with auto-sync on, ArgoCD may revert it before you have read the logs,
   so pause auto-sync for the check or run it in a quiet window.

   A `10.42.x.x` address, or a node's, means Envoy's Service is not `externalTrafficPolicy:
   Local` or another proxy stands in front: fix that, or `ULW_TRUSTED_PROXY_HOPS`, before prod.
5. Memory under load, in the sandbox only: `make e2e-load` (with `make e2e-up` run first)
   installs metrics-server, uploads through the route with 500 uploads in flight, samples
   `kubectl top pods --containers` every 10 s, and fails if a gateway or worker container goes
   over the limits in `overlays/*/video-gateway/deployment.yaml` and `video-worker/deployment.yaml`
   or restarts. Reports are written to `load-report/`; the `e2e` workflow runs it nightly and
   keeps them as an artifact. The check refuses every cluster but the sandbox and there is no
   variant for stage: a load generator must not be pointed at a shared cluster. On stage,
   record `kubectl -n apps-stage top pod -l app.kubernetes.io/name=video-gateway` at rest and
   after step 2, and compare it with the sandbox report. The limit and its derivation are in
   `overlays/*/video-gateway/deployment.yaml`.
6. Chat through the route. The sandbox's `chat` scenario (`make e2e-test`) joins a stream's live
   chat from six sockets across the three nodes and checks one message reaches them all; on
   stage, check the upgrade with a real token, kept in a file so it stays off the command line
   (`Authorization: Bearer <token>` on one line):

   ```sh
   curl -si --http1.1 --max-time 3 -H @stage-token.header -H 'Upgrade: websocket' \
     -H 'Connection: Upgrade' -H 'Sec-WebSocket-Version: 13' \
     -H "Sec-WebSocket-Key: $(openssl rand -base64 16)" https://<stage host>/rt | head -1
   ```

   It must print `HTTP/1.1 101 Switching Protocols` (curl then times out holding the socket,
   which is expected); without the header line it is `401`. A `404` means the route is not
   attached (`parentRefs`, as above). From a page on `https://stage.askedin.com` the cookie
   works too; from any other origin it is `403` (docs/integration/chat.md).

## 6. Rollback

The overlays pull the branch tag, so a rollback is a retag and a restart. Woodpecker also
pushed every build under its commit SHA:

```sh
crane tag git.askedin.com/askedin/askedin-monorepo/video-gateway:<good sha> development
crane tag git.askedin.com/askedin/askedin-monorepo/video-worker:<good sha> development
crane tag git.askedin.com/askedin/askedin-monorepo/chat:<good sha> development
crane tag git.askedin.com/askedin/askedin-monorepo/live-packager:<good sha> development
kubectl -n apps-stage rollout restart deployment/video-gateway
kubectl -n apps-stage rollout status deployment/video-gateway
kubectl -n apps-stage rollout restart deployment/video-worker
kubectl -n apps-stage rollout restart deployment/chat
```

Packagers already running keep their build; streams started after the retag pull the good one.
A rollback across a change of chat's node-channel version is a `Recreate` too (step 3).

(`docker pull`, `docker tag`, `docker push` do the same as `crane tag`.) Migrations only add
(docs/adr/0031), so the older build runs against the newer schema; the database is never rolled
back. For prod, the tag is `master` and the namespace `apps`. Revert the commit on the branch
too, or the next push redeploys it.

To take the plane out entirely: delete the HTTPRoute first (uploads stop at the edge), then
scale both deployments to 0. Uploads in progress resume once it is back. Chat the same way: its
HTTPRoute, then `deployment/chat` to 0; clients reconnect and resume from their last seq once it
is back.

## 7. The realtime plane: STUNner and LiveKit

Media from browsers enters through STUNner, a TURN server run as a Gateway API implementation
beside Envoy Gateway (docs/adr/0013); LiveKit is the SFU behind it (docs/adr/0020). LiveKit hands
each client a TURN credential minted from a secret it shares with STUNner (docs/adr/0037).

### Check the cluster first

```sh
# v1.x, from Envoy Gateway:
kubectl get crd gateways.gateway.networking.k8s.io \
  -o jsonpath='{.metadata.annotations.gateway\.networking\.k8s\.io/bundle-version}'
kubectl get svc -A | grep -w 3478        # nothing may hold UDP 3478 yet
```

STUNner's Gateway is a LoadBalancer Service on UDP 3478 with `externalTrafficPolicy: Local`, so
TURN clients should keep their own source address. On K3s, ServiceLB publishes it on the
node's address. Open UDP 3478 to the internet on the node's firewall, and nothing else: every
call's media arrives on that one port, and LiveKit's own UDP port (7882) stays inside the
cluster.

Whether ServiceLB really keeps the client's address is not tested anywhere before stage: the
sandbox has no ServiceLB and publishes a NodePort instead. The probe in "Verify on stage"
tells: `binding.mapped` must be the probing machine's public address. If it is a node or pod
address instead, calls still work (media goes through the relay, not the reflexive address),
but STUNner sees every client as the node, so its logs and any per-client limit lose the
client. The fix is the path the sandbox proves, a NodePort under `externalTrafficPolicy: Local`
with the node forwarding 3478 to it:

```sh
kubectl -n apps-stage annotate gateway stunner --overwrite \
  stunner.l7mp.io/service-type=NodePort 'stunner.l7mp.io/nodeport={"turn-udp": 31478}'
# on k8s-prod, persisted in its firewall configuration:
iptables -t nat -A PREROUTING -p udp --dport 3478 -j REDIRECT --to-ports 31478
```

Put the annotations in `overlays/stage/stunner/gateway.yaml` too, or ArgoCD reverts them, then
probe again. Running stunnerd in the host's network, the other usual fix, is ruled out: pods
may not use `hostNetwork` (docs/adr/0013).

### Secrets

STUNner holds one shared secret for the whole cluster, and LiveKit in each environment signs
its clients' TURN credentials with that same secret (docs/adr/0037). So `TURN_SECRET` is not a
per-environment value, even though it lives in both env files: a copy that differs breaks
every call of the environment whose LiveKit holds it.

New keys for `.env.stage` and `.env.prod` (names only):

```
TURN_SECRET              32+ random bytes, base64; the same value in both files
TURN_HOST                where browsers reach STUNner: the node's public IP, or a DNS name for it
LIVEKIT_KEYS             "<api key>: <api secret>", the secret at least 32 characters
REDIS_PASSWORD           32+ random characters, letters and digits; LiveKit's and egress's Redis
```

STUNner's secret is created by a script of its own, `scripts/create-turn-secret.sh`, run once
per cluster and never from `create-k8s-secrets.sh`, which runs per environment. It refuses when
the two env files disagree:

```sh
#!/usr/bin/env bash
# STUNner's shared TURN secret: one per cluster, used by stage and prod alike.
set -euo pipefail
cd "$(dirname "$0")/.."
turn_secret() { (set -a; source "$1"; printf '%s' "${TURN_SECRET:?TURN_SECRET missing from $1}"); }
stage=$(turn_secret .env.stage)
prod=$(turn_secret .env.prod)
if [[ $stage != "$prod" ]]; then
    echo "create-turn-secret: TURN_SECRET differs between .env.stage and .env.prod;" \
        "STUNner has one per cluster, so set both to the same value" >&2
    exit 1
fi
# From a file descriptor, so the secret never appears in a process list.
kubectl -n stunner-system create secret generic stunner-secrets \
    --from-literal=type=ephemeral --from-file=secret=<(printf '%s' "$stage") \
    --dry-run=client -o yaml | kubectl apply -f -
```

The secret carries no `ASKEDIN_ENV`: it belongs to neither environment. `stunner-system` must
exist first (the operator step below creates it).

Lines for `scripts/create-k8s-secrets.sh`, per environment as usual. Until prod's LiveKit is
deployed (below, "Prod"), guard them to stage; drop the guard in the same change that adds
`overlays/prod/livekit/` to the monorepo:

```sh
if [[ $NS == apps-stage ]]; then
  # Egress takes the pair as two values; both come from LIVEKIT_KEYS, so they cannot disagree.
  kubectl -n "$NS" create secret generic sfu-secrets \
    --from-literal=ASKEDIN_ENV="$ASKEDIN_ENV" \
    --from-literal=LIVEKIT_KEYS="$LIVEKIT_KEYS" \
    --from-literal=LIVEKIT_API_KEY="${LIVEKIT_KEYS%%: *}" \
    --from-literal=LIVEKIT_API_SECRET="${LIVEKIT_KEYS#*: }" \
    --from-literal=REDIS_PASSWORD="$REDIS_PASSWORD" \
    --from-literal=TURN_HOST="$TURN_HOST" \
    --from-literal=TURN_SECRET="$TURN_SECRET" \
    --dry-run=client -o yaml | kubectl apply -f -
fi
```

The call service gets the same `LIVEKIT_KEYS` pair once it ships. Add Redis, LiveKit and egress
to the rollout-restart list, in that order (LiveKit does not start without Redis); STUNner
rereads its secret by itself:

```sh
kubectl -n "$NS" rollout restart deployment/livekit-redis
kubectl -n "$NS" rollout status deployment/livekit-redis
kubectl -n "$NS" rollout restart deployment/livekit deployment/livekit-egress
```

To rotate `TURN_SECRET`: change it in both env files, run `scripts/create-turn-secret.sh`, then
`create-k8s-secrets.sh` for each environment, which restarts LiveKit. Calls in progress
reconnect once with credentials under the new secret.

### Install, once per cluster

STUNner's CRDs come from its chart at v1.2.1, checked against the hash `deploy/local/tools.sh`
pins:

```sh
curl -fsSLo stunner-crds.yaml https://raw.githubusercontent.com/l7mp/stunner-helm/08555494a2fdb53c0f8a0146cfa1c951dbb83f1b/helm/stunner/crds/stunner-crds.yaml
echo "720ab0c18e0e51b8cee18259685061e03cc0d3d01e90a0c0fc20c5144351b279  stunner-crds.yaml" | sha256sum -c
kubectl apply --server-side -f stunner-crds.yaml
kubectl apply -f stunner/operator.yaml
kubectl -n stunner-system rollout status deployment/stunner-gateway-operator-controller-manager
scripts/create-turn-secret.sh          # and create-k8s-secrets.sh for stage, as above
kubectl apply -f stunner/dataplane.yaml -f stunner/gatewayclass.yaml
```

Do not install the chart's own Gateway API CRDs: Envoy Gateway owns them, and a second copy at
another version would fight it. Then copy `overlays/stage/stunner/`, `overlays/stage/livekit/`
and `overlays/stage/livekit-redis/` (and `overlays/stage/livekit-egress/` for live streams)
into the monorepo's stage overlay tree like the others; ArgoCD applies them. LiveKit, Redis and
egress are pulled by digest, so Woodpecker has nothing to build for them.

### Verify on stage

```sh
kubectl get gatewayclass stunner-gatewayclass                 # ACCEPTED True
kubectl -n apps-stage get gateway stunner                     # PROGRAMMED True, ADDRESS the node's
kubectl -n apps-stage get udproutes.stunner.l7mp.io livekit \
  -o jsonpath='{.status.parents[0].conditions[*].type}={.status.parents[0].conditions[*].status}'
kubectl -n apps-stage get deploy stunner livekit livekit-redis livekit-egress   # 1/1 each
kubectl -n apps-stage logs deploy/livekit-egress | head      # connected to Redis, no errors
```

Egress has not run in the sandbox cluster (its image is ~5 GB; the local call suite runs it
under compose), so its pod settings here (uid 10001 with an empty home directory, a read-only
root filesystem, the service started without the image's PulseAudio entrypoint) are first
proven on stage: a stream relayed per step 9 whose playlist appears is the check.

From a machine outside the cluster (a laptop on another network), with the stage secret:

```sh
TURN_SECRET=$(set -a; . ./.env.stage; printf '%s' "$TURN_SECRET") \
  tests/cluster/turn_probe.py <TURN_HOST> 3478 \
  --permit <livekit pod IP> --forbid <any other pod IP> --forbid 127.0.0.1 --forbid <node IP>
```

The secret goes in through the environment, read from the env file, so it never reaches the
command line or the shell history.

It only sends STUN and TURN requests. Expect `binding.mapped` to be the machine's public
address (compare `curl -s https://ifconfig.me`): anything else means the node masquerades
the traffic and `externalTrafficPolicy` is not taking effect. Expect `allocate` a success
with `integrity: true` and a relayed address inside the cluster, the `permit` peer a success,
the `forbid` peer an error, and `wrong_password` and `expired` errors (400 or 401).

### Rollback

ArgoCD owns the stage overlays, so anything deleted by hand comes back at the next sync. Revert
or remove the overlay on `development` first: take out `overlays/stage/stunner/` to stop media
(STUNner then relays to nothing and calls stop at once), and `overlays/stage/livekit/`,
`livekit-redis/` and `livekit-egress/` as well to remove the plane. Once ArgoCD has synced, check both are gone:

```sh
kubectl -n apps-stage get gateway,udproutes.stunner.l7mp.io,deploy -l app.kubernetes.io/part-of=ulw
```

To stop media before the sync lands, delete the UDPRoute by hand as well
(`kubectl -n apps-stage delete udproutes.stunner.l7mp.io livekit`). The operator removes the
stunnerd Deployment and Service when its Gateway goes. If nothing else uses STUNner, delete
`stunner/*.yaml` and the CRDs last; they are not in any overlay.

### Prod

Prod runs the same plane from `overlays/prod/stunner/`, `overlays/prod/livekit/`,
`overlays/prod/livekit-redis/` and `overlays/prod/livekit-egress/`, in `apps`. **Apply none of
it before its phase is tagged on stage**: STUNner, LiveKit and Redis after phase-4 (one-to-one
calls), egress after phase-6 (live), each once stage has passed "Verify on stage" above. Until
then the prod overlays stay out of the monorepo's prod tree. What differs from stage, and why:

- **TURN on UDP 3479, not 3478.** Stage and prod share k8s-prod's one node, ServiceLB publishes
  each LoadBalancer Service on that node's address, and stage's Gateway already holds UDP 3478
  there; a second Service on 3478 would stay pending. Prod gets a Gateway of its own rather
  than a route on stage's, so its relay reaches prod's LiveKit only. LiveKit's
  `rtc.turn_servers` names 3479 to match.
- **Signalling on Askedin's prod hostnames.** Prod's HTTPRoute names `askedin.com` and
  `www.askedin.com`; stage's names none and so also matches them, and the route that names the
  hostname wins there. A call or publisher ticket for prod must name one of these hosts.
- **Its own LiveKit keys and `TURN_HOST`**, in `sfu-secrets` in `apps`; the TURN secret is the
  cluster's one, the same value as stage's (Secrets, above).
- **LiveKit the same size as stage**: one replica, 500m CPU and 256Mi requested, 2 CPU and 1Gi
  at most: about 126 concurrent 1:1 calls (docs/integration/calls.md, Capacity). STUNner's
  pods come from the cluster's one `Dataplane`, so prod's stunnerd is sized as stage's (500m,
  128Mi). Redis is stage's too (50m, 64Mi).
- **Egress for two concurrent streams** where stage takes one: a 5-core limit, since egress
  admits a stream only while 2 of its cores are idle under its 80% ceiling, and 768Mi
  requested, 1536Mi at most (~300 MB per stream, docs/adr/0053). Raise the limit by 2.5 cores
  per further concurrent stream, if the node has them.

Nothing in prod's chat, gateway or packager overlays changes for it: no ULW service reads
LiveKit's address or keys yet. The call and stream services, when they ship, read the same
`LIVEKIT_KEYS` pair and LiveKit's in-cluster address, `http://livekit.apps.svc.cluster.local:7880`
(`.apps-stage.` on stage).

Owner steps, all Askedin's, in this order:

1. **Capacity.** Prod's calls plane requests 1.05 CPU and 448Mi more on the node (LiveKit
   500m/256Mi, stunnerd 500m/128Mi, Redis 50m/64Mi), and egress 1 CPU and 768Mi more (and up
   to 5 CPU while streams run). Stage's plane takes 1.05 CPU and 448Mi, and 1 CPU and 512Mi for
   its egress. Check against step 1's numbers before applying:

   ```sh
   kubectl describe nodes | sed -n '/Allocated resources/,/Events/p'
   ```

2. **The port.** Nothing may hold UDP 3479 yet; open it to the internet on k8s-prod's firewall,
   and nothing else (LiveKit's 7882 stays inside the cluster, as on stage):

   ```sh
   kubectl get svc -A | grep -w 3479        # must print nothing
   # on k8s-prod, persisted in its firewall configuration:
   iptables -A INPUT -p udp --dport 3479 -j ACCEPT
   ```

   If stage needed the NodePort fix of "Check the cluster first", prod needs it too, on a port
   of its own, with both annotations in `overlays/prod/stunner/gateway.yaml`:

   ```sh
   kubectl -n apps annotate gateway stunner --overwrite \
     stunner.l7mp.io/service-type=NodePort 'stunner.l7mp.io/nodeport={"turn-udp": 31479}'
   iptables -t nat -A PREROUTING -p udp --dport 3479 -j REDIRECT --to-ports 31479
   ```

   If Askedin would rather give prod an address of its own, prod can keep 3478 on that address:
   set `spec.addresses` on `overlays/prod/stunner/gateway.yaml`'s Gateway to it, put the
   listener and `rtc.turn_servers` in `overlays/prod/livekit/deployment.yaml` back to 3478,
   and open 3478 on that address instead.

3. **A DNS name for TURN** (optional; the node's public IP works as `TURN_HOST` too). An A
   record such as `turn.askedin.com` pointing at k8s-prod's public address. TURN over UDP needs
   no certificate. Find the address with:

   ```sh
   kubectl get nodes -o wide          # EXTERNAL-IP, or the address the firewall publishes
   ```

4. **Prod's keys in `.env.prod`** (`TURN_SECRET` is already there, equal to stage's; see
   Secrets). Generate a key pair of prod's own, never stage's:

   ```sh
   printf 'LIVEKIT_KEYS="%s: %s"\n' "API$(openssl rand -hex 6)" "$(openssl rand -base64 36)" >> .env.prod
   printf 'REDIS_PASSWORD=%s\n' "$(openssl rand -hex 24)" >> .env.prod
   echo 'TURN_HOST=<turn.askedin.com or the node public IP>' >> .env.prod
   ```

   Then drop the `apps-stage` guard from `create-k8s-secrets.sh` (Secrets, above) and run it
   for prod, which creates `sfu-secrets` in `apps`:

   ```sh
   scripts/create-k8s-secrets.sh          # as you run it for prod (NS=apps)
   kubectl -n apps get secret sfu-secrets -o jsonpath='{.data}' | python3 -c \
     'import json,sys; print(sorted(json.load(sys.stdin)))'
   # ['ASKEDIN_ENV', 'LIVEKIT_API_KEY', 'LIVEKIT_API_SECRET', 'LIVEKIT_KEYS', 'REDIS_PASSWORD',
   #  'TURN_HOST', 'TURN_SECRET']
   ```

   Run `scripts/create-turn-secret.sh` again only if `TURN_SECRET` changed; it refuses when
   the two env files disagree.

5. **Apply**, after the phase tag (above). Copy `overlays/prod/stunner/`,
   `overlays/prod/livekit/` and `overlays/prod/livekit-redis/` into the monorepo's prod overlay
   tree after phase-4, and `overlays/prod/livekit-egress/` after phase-6; ArgoCD applies them
   on `master`. Add Redis, LiveKit and egress to prod's rollout-restart list, as on stage.

6. **Verify**, as on stage, with `apps` for `apps-stage` and port 3479:

   ```sh
   kubectl -n apps get gateway stunner                     # PROGRAMMED True, ADDRESS the node's
   kubectl -n apps get udproutes.stunner.l7mp.io livekit \
     -o jsonpath='{.status.parents[0].conditions[*].type}={.status.parents[0].conditions[*].status}'
   kubectl -n apps get deploy stunner livekit livekit-redis   # 1/1 each; livekit-egress too after phase-6
   kubectl -n apps get httproute livekit \
     -o jsonpath='{.status.parents[0].conditions[*].type}={.status.parents[0].conditions[*].status}'
   ```

   From a machine outside the cluster:

   ```sh
   TURN_SECRET=$(set -a; . ./.env.prod; printf '%s' "$TURN_SECRET") \
     tests/cluster/turn_probe.py <TURN_HOST> 3479 \
     --permit <prod livekit pod IP> --forbid <stage livekit pod IP> --forbid 127.0.0.1 \
     --forbid <node IP>
   ```

   The stage LiveKit pod must be forbidden: prod's relay reaches only prod's LiveKit. Expect
   the same results as on stage otherwise. Then the signalling route, which must answer
   LiveKit, not 404:

   ```sh
   curl -s -o /dev/null -w '%{http_code}\n' https://askedin.com/rtc/validate   # LiveKit's 401, not 404
   ```

Rollback is stage's with `apps` and `master`: take `overlays/prod/stunner/` (and
`overlays/prod/livekit/`, `livekit-redis/` and `livekit-egress/`) out of the prod overlay tree, and to stop media before the sync lands,
`kubectl -n apps delete udproutes.stunner.l7mp.io livekit`. Stage is untouched by either.

## 8. Askedin signing key rotation

Askedin's auth-service rotates its RSA signing key with no overlap: one transaction creates the
new key and deactivates the old one, and the old `kid` leaves the JWKS at once. The gateway and
chat each cache the key set and remembers verified tokens for up to 15 minutes each,
so left alone it accepts tokens under the old key for up to 15 minutes after a rotation
(docs/integration/auth.md, Key rotation). SIGHUP refetches the key set at once and, when that
fetch succeeds, replaces the keys and forgets every remembered token (docs/adr/0082).

Askedin's rotation runbook restarts the services that verify its tokens. Add ULW to it: once the
rotation has committed, in the same environment (`apps-stage` for stage, `apps` for prod):

```sh
NS=apps-stage   # apps for prod
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

If Askedin's JWKS cannot be reached, the SIGHUP drops nothing yet: `auth_cache_drop_pending`
stays at 1 and tokens under the old key keep working until a fetch succeeds, which then
completes the drop. That is the right trade for a routine rotation. For a suspected key
compromise during a JWKS outage, use
`kubectl -n "$NS" rollout restart deployment/video-gateway deployment/chat` instead: new pods
start with no keys and refuse every token (`503`) until a fetch succeeds, which fails closed.

## 9. Live streams: the packager (stage)

A live stream reaches viewers as HLS that its packager writes to the bucket (docs/adr/0046),
from LiveKit's recorder (egress), which the stream service starts once the publisher's WHIP
POST has succeeded (docs/adr/0053). A packager is one process per stream, so on the cluster it
is one Job per stream, made from `live-packager/job.yaml` (docs/adr/0083). What is not here
yet, and is needed before a stream can go out:

- **LiveKit egress and its Redis**: `overlays/*/livekit-egress/` and `overlays/*/livekit-redis/`
  (step 7). Egress's pods carry `app.kubernetes.io/name: livekit-egress`, the only pods the
  packager's NetworkPolicy admits, and LiveKit's configuration names the same Redis. Stage's
  egress takes one stream at a time, prod's two; a further one is refused (`Unavailable` from
  the relay) until one ends.
- **The stream service**, which makes each stream's Job and Secret, calls the relay with the
  stream's passphrase, and records the stream's chat live (step 3). Until it exists, the steps
  below start a packager by hand.

The relay's packager address (the SFU adapter's `packager_srt`, ADR-0053) is
`srt://{stream}.live-packager.apps-stage.svc.cluster.local:9000` on stage, and `.apps.` in
place of `.apps-stage.` on prod: `overlays/*/live-packager/service.yaml` gives each packager pod
that name.

### Secrets and the database role

New keys for `.env.stage` and `.env.prod` (names only):

```
VIDEO_LIVE_DATABASE_URL               postgresql://… for the ulw database, as the role below
VIDEO_LIVE_R2_ACCESS_KEY_ID           R2 token for the packager: object read and write on the bucket
VIDEO_LIVE_R2_SECRET_ACCESS_KEY
```

The packager needs only a few rights on the database (docs/integration/operations-contract.md),
so it gets a role of its own rather than the gateway's. Once, as a superuser, after the
gateway's migrations have run (`ulw_prod_live` and `ulw_prod` for prod):

```sql
CREATE ROLE ulw_stage_live LOGIN;
\password ulw_stage_live
\connect ulw_stage
SET ROLE ulw_stage;   -- the tables' owner grants on them
GRANT SELECT, INSERT ON live_recordings TO ulw_stage_live;
GRANT SELECT (id), INSERT ON videos TO ulw_stage_live;
GRANT INSERT ON jobs TO ulw_stage_live;
GRANT USAGE ON SEQUENCE jobs_id_seq TO ulw_stage_live;
```

Lines for `scripts/create-k8s-secrets.sh`:

```sh
kubectl -n "$NS" create secret generic live-packager-secrets \
  --from-literal=ASKEDIN_ENV="$ASKEDIN_ENV" \
  --from-literal=ULW_DATABASE_URL="$VIDEO_LIVE_DATABASE_URL" \
  --from-literal=ULW_R2_ACCOUNT_ID="$VIDEO_R2_ACCOUNT_ID" \
  --from-literal=ULW_BUCKET="$VIDEO_R2_BUCKET" \
  --from-literal=ULW_S3_ACCESS_KEY_ID="$VIDEO_LIVE_R2_ACCESS_KEY_ID" \
  --from-literal=ULW_S3_SECRET_ACCESS_KEY="$VIDEO_LIVE_R2_SECRET_ACCESS_KEY" \
  --dry-run=client -o yaml | kubectl apply -f -
```

Nothing to restart: each packager reads the secret when its Job starts.

The bucket keeps a stream's segments under `live/<stream>/`, and the recording is read back from
them after the stream ends, so they need an expiry of days, not hours (operations-contract.md).
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
characters, and goes only into its Secret and the relay request:

```sh
NS=apps-stage TAG=development          # apps and master for prod
STREAM=launch-2026
OWNER=<the broadcaster's Askedin user id (sub)>
kubectl -n "$NS" create secret generic "live-packager-$STREAM" \
  --from-file=ULW_LIVE_SRT_PASSPHRASE=<(openssl rand -hex 24 | tr -d '\n')
ULW_NAMESPACE=$NS ULW_IMAGE_TAG=$TAG ULW_STREAM_ID=$STREAM ULW_STREAM_OWNER=$OWNER \
  envsubst '${ULW_NAMESPACE} ${ULW_IMAGE_TAG} ${ULW_STREAM_ID} ${ULW_STREAM_OWNER}' \
  < live-packager/job.yaml | kubectl apply -f -
kubectl -n "$NS" logs -f "job/$STREAM"
```

Name the four variables to `envsubst` exactly as above: the template also holds `$(POD_IP)`,
which is Kubernetes' to expand, not the shell's. The log's first line names the stream, the
storage and `ingest=<pod IP>:9000`; from then on the packager waits for its one SRT caller. The
gateway serves the stream's playlist at `GET /api/v1/live/launch-2026/index.m3u8` once the first
segment is stored (docs/integration/live.md).

The stream ends when its caller goes; the packager then writes `EXT-X-ENDLIST`, queues the
recording as a video and logs `recording: queued as video <id>`, and the Job completes. To end
it from the cluster instead, send the packager SIGUSR1
(`kubectl -n "$NS" exec "job/$STREAM" -c packager -- sh -c 'kill -USR1 1'`; `live_packager`
is PID 1 in its container). A SIGTERM (a node drain, a
`kubectl delete pod`) drains instead: the process exits 0, the Job counts as complete, and the
stream is left to be continued by a new packager for the same stream id, which whoever started
the stream must start (the template's `backoffLimit` restarts only failures). The Job and its
pod are removed a day after they finish; delete the stream's Secret with them:

```sh
kubectl -n "$NS" delete secret "live-packager-$STREAM"
```

Every packager runs ffmpeg under the worker's sandbox, so it needs what step 1 checks for the
worker (user namespaces) and step 2's seccomp profile on the node it lands on.
