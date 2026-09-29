# Deploying the VOD plane on Askedin

For the person with access to Askedin's cluster. Nothing in this repository applies these files
to it; every command below is yours to run. Stage first, prod the same way a release later.

What ships:

| Path | What |
|---|---|
| `overlays/{stage,prod}/video-gateway/` | Deployment (2 replicas, migrations in its init container), Service, HTTPRoute + BackendTrafficPolicy, NetworkPolicy |
| `overlays/{stage,prod}/video-worker/` | Deployment (1 replica), NetworkPolicy; no Service, no route |
| `seccomp/ulw-worker.json` | The worker's seccomp profile, installed on the node (step 2) |
| `woodpecker.yml` | Builds and pushes both images, then `rollout restart`; never applies a manifest |

`chat` and `live-packager` have no overlays yet: their binaries do not exist. STUNner comes with
the realtime plane.

Open decisions, yours: whether this builds inside the Askedin monorepo or pushes from this
repository (the image names `git.askedin.com/askedin/askedin-monorepo/<svc>` assume the
monorepo), and the real `JWKS_URL` and `JWT_ISSUER`.

## 1. Check the cluster can run the worker

The worker gives each ffmpeg its own namespaces (docs/adr/0034), which needs pods in user
namespaces. On k8s-prod:

```sh
kubectl version                        # server v1.33 or later
uname -r                               # 6.3 or later (idmapped mounts on overlayfs)
k3s --version; runc --version          # containerd 2.x, runc 1.2 or later
```

The gateway's NetworkPolicy admits only Envoy's data plane, found by labels. Confirm them, or
edit `overlays/*/video-gateway/networkpolicy.yaml` before the first apply:

```sh
kubectl get pods -A -l app.kubernetes.io/component=proxy,app.kubernetes.io/managed-by=envoy-gateway \
  -o custom-columns=NS:.metadata.namespace,NAME:.metadata.name   # expect namespace envoy-gateway-system
```

It also admits Prometheus from namespace `monitoring` to scrape `/metrics`. If Prometheus runs
elsewhere, change the second `from` in the same files:

```sh
kubectl get pods -A -l app.kubernetes.io/name=prometheus -o custom-columns=NS:.metadata.namespace
```

Check the node has room. Both environments run on k8s-prod's 8 vCPU / 24 GB, and the new
requests are, per environment, 2 x 500m CPU and 2 x 600Mi for the gateways plus the worker's
2Gi, and 1 CPU / 10Gi of scratch (stage) or 2 CPU / 30Gi (prod) for the worker: 5 CPU, 6.4Gi of
memory and 40Gi of ephemeral storage for both. Compare with what is already allocated:

```sh
kubectl describe nodes | sed -n '/Allocated resources/,/Events/p'
df -h /var/lib/kubelet                 # the scratch emptyDirs live here
```

If the node cannot take it, lower the prod worker's CPU request before the first prod apply;
the gateways' memory requests are the budget of docs/adr/0027 and should not move.

## 2. Install the worker's seccomp profile on k8s-prod

Once per node, and again whenever `seccomp/ulw-worker.json` changes:

```sh
sudo install -D -m 0644 seccomp/ulw-worker.json /var/lib/kubelet/seccomp/profiles/ulw-worker.json
```

Without it the worker pod stays in `CreateContainerError`.

## 3. Secrets

New keys for `.env.stage` and `.env.prod` (names only). The R2 bucket should have a lifecycle
rule aborting incomplete multipart uploads after 7 days; the gateway expires its uploads after 6.

```
VIDEO_DATABASE_URL                    postgresql://… for the ulw database; the role needs DDL (docs/adr/0033)
VIDEO_R2_ACCOUNT_ID
VIDEO_R2_BUCKET
VIDEO_GATEWAY_R2_ACCESS_KEY_ID        R2 token for the gateway: object read and write on the bucket
VIDEO_GATEWAY_R2_SECRET_ACCESS_KEY
VIDEO_WORKER_R2_ACCESS_KEY_ID         R2 token for the worker: object read and write on the bucket
VIDEO_WORKER_R2_SECRET_ACCESS_KEY
VIDEO_JWKS_URL                        https://… Askedin's JWKS; must be https
VIDEO_JWT_ISSUER                      the iss Askedin's auth-service puts in its tokens
```

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
  --from-literal=JWKS_URL="$VIDEO_JWKS_URL" \
  --from-literal=JWT_ISSUER="$VIDEO_JWT_ISSUER" \
  --dry-run=client -o yaml | kubectl apply -f -
kubectl -n "$NS" create secret generic video-worker-secrets \
  --from-literal=ASKEDIN_ENV="$ASKEDIN_ENV" \
  --from-literal=ULW_DATABASE_URL="$VIDEO_DATABASE_URL" \
  --from-literal=ULW_R2_ACCOUNT_ID="$VIDEO_R2_ACCOUNT_ID" \
  --from-literal=ULW_BUCKET="$VIDEO_R2_BUCKET" \
  --from-literal=ULW_S3_ACCESS_KEY_ID="$VIDEO_WORKER_R2_ACCESS_KEY_ID" \
  --from-literal=ULW_S3_SECRET_ACCESS_KEY="$VIDEO_WORKER_R2_SECRET_ACCESS_KEY" \
  --dry-run=client -o yaml | kubectl apply -f -
```

Add both deployments to the script's `kubectl rollout restart` list, so a secret change reaches
them:

```sh
kubectl -n "$NS" rollout restart deployment/video-gateway deployment/video-worker
```

The worker gets no JWT settings at all.

The role and database on k8s-prod's Postgres, once, as a superuser (psql prompts for the
password with `\password`; it never goes on a command line):

```sql
CREATE ROLE ulw_stage LOGIN;           -- ulw_prod for prod
\password ulw_stage
CREATE DATABASE ulw_stage OWNER ulw_stage;
```

The role owns its database, which gives the migrations their DDL rights (docs/adr/0033).
`VIDEO_DATABASE_URL` is then `postgresql://ulw_stage:<password>@<host>:5432/ulw_stage`, with the
password percent-encoded.

## 4. Pipeline and first deploy

1. Copy `overlays/stage/*` and `overlays/prod/*` into the monorepo's overlay tree, and
   `woodpecker.yml` into its pipeline directory. Give Woodpecker the secrets `registry_user`,
   `registry_password` and `kubeconfig`; the kubeconfig's user needs only `get`, `list`, `watch`
   and `patch` on deployments and `get`, `list`, `watch` on replicasets, in `apps` and
   `apps-stage` (`rollout restart` and `rollout status`). The image steps run the buildx plugin,
   which Woodpecker 3 runs privileged only when the server lists it: add the exact reference
   from `woodpecker.yml` (tag and digest) to the server's `WOODPECKER_PLUGINS_PRIVILEGED`.
2. Push to `development`. Woodpecker pushes `video-gateway:development` and
   `video-worker:development`; the restart step fails on the first run because the deployments
   do not exist yet. That is expected.
3. Bootstrap stage once:

   ```sh
   scripts/apply-k8s-overlay.sh stage --with-deployments
   ```

   `--with-deployments` applies every service's `deployment.yaml` in the stage overlay, not
   just these two. Any service whose live image or replica count was changed outside git (a
   `kubectl set image`, a manual scale) is reset to what git says. Check first with
   `kubectl -n apps-stage diff -f <each other service's deployment.yaml>`, and run it when a
   reset would not hurt.
4. From then on ArgoCD syncs the manifests and Woodpecker only restarts.

The route serves `/api/v1/uploads` and `/api/v1/videos` on every hostname of
`askedin-gateway`. If the video plane gets a hostname of its own, add `hostnames:` to both
`httproute.yaml` files.

## 5. Verify on stage (M14)

1. ArgoCD shows the stage application Synced and Healthy; then:

   ```sh
   kubectl -n apps-stage get deploy video-gateway video-worker      # 2/2 and 1/1
   kubectl -n apps-stage logs deploy/video-gateway -c migrate        # "applied …" or "schema is up to date"
   kubectl -n apps-stage get httproute video-gateway \
     -o jsonpath='{.status.parents[*].conditions[?(@.type=="Accepted")].status}'   # True
   ```

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
4. Memory under load: **not available yet.** M14 asks for `kubectl top` staying inside the
   derived limits while 500 uploads run against stage, but no load generator that uploads
   through a route exists yet (`ulw_gateway_load` starts its own gateway; it is part B of M14).
   Until then, record `kubectl -n apps-stage top pod -l app.kubernetes.io/name=video-gateway`
   at rest and after step 2, and leave this check open. The limit and its derivation are in
   `overlays/*/video-gateway/deployment.yaml`.

## 6. Rollback

The overlays pull the branch tag, so a rollback is a retag and a restart. Woodpecker also
pushed every build under its commit SHA:

```sh
crane tag git.askedin.com/askedin/askedin-monorepo/video-gateway:<good sha> development
crane tag git.askedin.com/askedin/askedin-monorepo/video-worker:<good sha> development
kubectl -n apps-stage rollout restart deployment/video-gateway
kubectl -n apps-stage rollout status deployment/video-gateway
kubectl -n apps-stage rollout restart deployment/video-worker
```

(`docker pull`, `docker tag`, `docker push` do the same as `crane tag`.) Migrations only add
(docs/adr/0033), so the older build runs against the newer schema; the database is never rolled
back. For prod, the tag is `master` and the namespace `apps`. Revert the commit on the branch
too, or the next push redeploys it.

To take the plane out entirely: delete the HTTPRoute first (uploads stop at the edge), then
scale both deployments to 0. Uploads in progress resume once it is back.
