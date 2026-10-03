# 0083. Chat and the live packager on the cluster: a Deployment of three, and a Job per stream

Status: Accepted
Date: 2026-10-03
Amends: ADR-0081 (chat_server now has an image, which links jemalloc); ADR-0053's "Deployment"
(the packager's manifests ship; egress's still do not)

## Context

The image (`deploy/docker/Dockerfile`) and Askedin's manifests (`deploy/askedin`) covered the
gateway, the worker and the upload reaper only. `chat_server` (ADR-0019: "deployed as `chat`
with 3 replicas") and `live_packager` (ADR-0046) had neither an image target nor a manifest,
so neither could be deployed, and ADR-0081's allocator decision reached no running process.

The two differ in shape:

- **Chat** is a long-running service. Its replicas share Postgres, each room has one owner
  (ADR-0015), and the others reach it over the node channel, a TCP port bound to the pod's own
  address and found through `chat_nodes` (ADR-0035), which was designed for "a Deployment, so
  replicas have neither stable names nor a fixed list of peers". It needs `ULW_NODE_ADDRESS`,
  `ULW_NODE_SECRET`, Askedin's JWKS and issuer, and the origins Askedin gave for its socket
  (docs/integration/auth.md). Its pod is sized at 1 GiB, with about 820 MiB at the very worst
  inside it, kernel buffers included (ADR-0036, operations-contract.md).
- **The live packager** is one process per stream: it takes the stream id, the owner and the
  SRT passphrase at start, accepts exactly one SRT caller, exits 0 once the stream has ended and
  its recording is queued (ADR-0055), exits non-zero for a restart to finish what failed, and
  exits 0 on SIGTERM, leaving the stream to be continued. It runs ffmpeg through ulw_sandbox,
  so it needs the worker's pod setup (ADR-0032), and its scratch root must exist, owned by its
  user, before it starts. LiveKit's recorder dials it at `packager_srt`, a URL with an optional
  `{stream}` in the host (ADR-0053). Nothing starts a packager yet: the stream service that
  will does not exist.

k8s-prod is one node running both environments (deploy/askedin/RUNBOOK.md, step 1).

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Chat as a StatefulSet (stable names `chat-0..2`, a headless Service) | Stable node ids and addresses | Rejected: the node channel finds peers in `chat_nodes` by design, and a restarted pod coming back under a new name is already handled (ADR-0035); a StatefulSet adds ordered rollouts and nothing chat uses |
| Chat as a Deployment of 3, `ULW_NODE_ADDRESS=$(POD_IP):9201`, the node name from `HOSTNAME` | What ADR-0019 and ADR-0035 describe; mirrors the gateway's Deployment | Accepted |
| A PodDisruptionBudget for chat | ADR-0019 notes a drain can take two replicas at once without one | Rejected for now: on a one-node cluster every replica goes with the node anyway, and a budget would only make `kubectl drain` wait forever. Reopen when the cluster has a second node |
| The packager as a Deployment per environment | The gateway's and worker's shape | Rejected: a process serves one stream, named at start. A Deployment restarts it after the stream ends, into a run that finds the stream already recorded and exits again, forever |
| A pool of packagers that claim streams | No per-stream object | Rejected: the binary takes its stream at start, and a pool needs a claim protocol the packager does not have |
| One Job per stream from a template, `restartPolicy: OnFailure` | Matches the process: runs to completion, restarts only failures in place (the pod and its address stay), is removed after a day | Accepted |
| A Service per stream for the recorder to dial (`live-{stream}`) | A plain ClusterIP name | Rejected: one more object per stream for the stream service to make and remove; a headless Service plus each pod's `hostname`/`subdomain` names every packager pod in DNS with one long-lived object |
| The Job template inside the overlays | One place for every manifest | Rejected: ArgoCD applies every file there, and would start a packager for a placeholder stream |
| The packager on the gateway's Ubuntu image | One runtime image less | Rejected: it runs the worker's ffmpeg, which must be Debian's (ADR-0074); the worker's Debian stage is shared instead |
| An HTTPRoute for the packager | Every other service has one | Rejected: nothing enters it from outside; only the recorder dials it, inside the cluster |

## Decision

- **Images.** `deploy/docker/Dockerfile` gains two targets. `chat`: the Ubuntu runtime stage
  plus `libjemalloc2=5.3.0-2build1`, with `chat_server`, exposing 9101 (clients) and 9201 (the
  node channel). `live-packager`: the worker's Debian stage (now `ffmpeg-runtime`, shared by both
  so an ffmpeg bump reaches both), with `live_packager` and `ulw_sandbox` beside it, and
  `/var/cache/ulw-live` made 0700 and owned by uid 10001. The build stage installs
  `libjemalloc-dev=5.3.0-2build1` and configures `-DULW_JEMALLOC=ON`, which links jemalloc
  into chat_server alone (ADR-0081); every other binary keeps glibc's malloc. Both images run as
  10001, are built, checked for hardening and gated by Trivy in the e2e sandbox job as the
  other two are (ADR-0072), and are pushed by `woodpecker.yml`; the trixie workflow loads the
  packager image's binaries on trixie as it does the worker's.
- **Chat** (`overlays/{stage,prod}/chat/`): a Deployment of 3 (rolling, one surge, none
  unavailable), the gateway's pod security (non-root 10001, read-only root, every capability
  dropped, RuntimeDefault seccomp, so `ULW_REACTOR=epoll`), probes on `/healthz` and `/readyz`,
  a 5 s preStop and a 20 s grace period (chat's 5 s drain plus margin), memory request 832Mi
  and limit 1Gi. Askedin's JWKS, cookie, origins and trusted proxies are in the overlay; the
  database URL, node secret and stage's issuer in `chat-secrets`. A ClusterIP Service on the
  client port, an HTTPRoute for `GET /rt` only (Exact, no request timeout), and a NetworkPolicy
  that admits Envoy and `monitoring` to the client port and chat pods alone to the node port,
  and lets chat reach DNS, the other chat pods' node port, 5432 and 443.
- **Live packager** (`overlays/{stage,prod}/live-packager/` and `live-packager/job.yaml`): the
  overlays hold what lasts, a headless Service `live-packager` (UDP 9000, not-ready addresses
  published) and a NetworkPolicy admitting UDP 9000 only from pods labelled
  `app.kubernetes.io/name: livekit-egress`, and letting the packager reach DNS and, outside the
  pod network, 5432 and 443. Each stream is a Job named by its stream id, from the template,
  whose pod has `hostname: <stream>` and `subdomain: live-packager`, so the relay's
  `packager_srt` is `srt://{stream}.live-packager.<namespace>.svc.cluster.local:9000`; it binds
  its listener to the pod's own address, takes the store and database credentials from
  `live-packager-secrets` (its own R2 token and a database role with only the grants
  operations-contract.md lists) and its passphrase from the stream's own Secret, and runs in
  the worker's pod setup (`hostUsers: false`, the worker's seccomp profile, `procMount:
  Unmasked`) with an emptyDir at `/var/cache/ulw-live`. Since a stream id then names a pod, it
  must be a DNS label, as ADR-0053 already requires when `{stream}` is in `packager_srt`.
- The sandbox (`deploy/local`) runs the chat overlay as it ships, patched only for what kind
  needs (images, the mock JWKS and its CA, the pod network, a smaller CPU request), and the
  e2e `chat` scenario joins a stream's live chat from six sockets through the route and checks
  one message reaches them all, and that a pod beside chat cannot reach its node port. The
  packager's Service and NetworkPolicy are applied there too; its Job template is validated
  (kubeconform, and a server-side dry run) as filled in for a sample stream, but no packager
  runs in the sandbox, since no egress does.

## Consequences

- Chat and the packager can be deployed; ADR-0081's jemalloc now ships in the chat image, and
  its vulnerabilities are gated with the image's (ADR-0072).
- Three chat pods ask for 2.4Gi and three quarters of a core per environment on a node that
  also carries everything else; the CPU number is not measured (RUNBOOK, step 1).
- On one node the three chat replicas share one failure domain, and no PodDisruptionBudget
  protects them; ADR-0019's "two thirds" is all of chat there.
- A packager drained by SIGTERM (a node drain, an eviction) exits 0 and its Job completes: the
  stream is left to be continued, and whoever started it must start a new packager for the
  same stream id. Kubernetes does not do it.
- A stream cannot go out until LiveKit egress and its Redis are deployed (not in these
  overlays) and something makes each stream's Job, Secret and relay: by hand per RUNBOOK step 9
  until the stream service exists. That service needs RBAC to create Jobs and Secrets in its
  namespace, which nothing here grants.
- Stream ids used on the cluster are DNS labels, narrower than what the playback URL accepts.
- The packager image is pulled by branch tag like the others; a stream started by the stream
  service should name it by digest (RUNBOOK, 4a).
- Reopen when the cluster gains a second node (a chat PodDisruptionBudget, spreading), when
  the stream service exists (it owns starting, continuing and cleaning up packagers), or if
  packager start-up latency per stream turns out too high for a Job per stream.
