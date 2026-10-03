# 0092. The stream service lives in the gateway, starts each stream's packager as a Job, and goes live on the owner's word

Status: Accepted
Date: 2026-10-03
Amends: ADR-0053 (who relays, and when), ADR-0070 (a stream's chat closes when it ends), ADR-0055 (who records a drained stream), ADR-0083
(what starts a packager, and the RBAC it needs)

## Context

Everything a live stream needs existed except the product flow that strings it together.
ADR-0053 gives a broadcaster a publisher ticket and relays the publisher to a packager, ADR-0046
and ADR-0047 package it, ADR-0059 serves its playlist, ADR-0055 records it, ADR-0083 ships a Job
template for its packager. Each of them defers the same component: "the stream service", which
issues tickets only while a stream is its owner's current one, starts the packager, calls the
relay once the publisher's WHIP POST has succeeded, tells viewers where to watch, ends the
stream, and maps the stream to its recording. Until now a test harness (`tests/call`'s
`ulw_call_harness`) or an operator (RUNBOOK step 9) played that part.

What had to be settled, from the code as it stands:

- **What the relay needs.** LiveKit's participant egress looks for its participant for 30 s and
  then fails (ADR-0053), so the relay must follow the publisher's POST, and the packager's SRT
  listener must be up when egress dials it. A Job's pod takes seconds to schedule and pull.
- **What a packager needs.** The stream id (a DNS label on the cluster, ADR-0083), the owner, and
  the SRT passphrase in its own Secret. A packager waits for its one caller with no limit, so a
  relay that never comes leaves it waiting until the Job's 13-hour deadline.
- **What the platform can observe.** A publisher's WHIP DELETE, or its silence, reaches LiveKit
  only; the stream's end is visible to us as the packager's `EXT-X-ENDLIST` within a second and
  as its exit after the recording, minutes later. No LiveKit webhook receiver exists.
- **The live documentation.** live.md already fixes the ticket's fields (`url`, `token`,
  `expires_at`), the fresh ticket before every WHIP request after the POST, one source per
  stream, the playlist route and who may watch (any signed-in user, ADR-0059), and the
  recording's ownership (the broadcaster's). It left the request for a ticket open.

## Options

**Where the service runs**

| Option | Why it was tempting | Verdict |
|---|---|---|
| A service of its own (a binary and a Deployment) | Its own failure domain and scaling | Rejected: it would duplicate the gateway's authentication, cookie and CSRF rules, rate limits, request log and playlist cache, for a handful of requests per stream per minute |
| In chat_server, over the room WebSocket, as ADR-0050 plans for call tickets | Calls will get their tickets there | Rejected: a broadcaster's encoder setup is HTTP-shaped (OBS, a page), the playlist and the recording are the gateway's already, and a WHIP client asks for a fresh ticket before each PATCH, which is a request/response, not a socket's message |
| In the gateway, as five more routes under `/api/v1/live` | The routes beside the playlist they point to; the gateway's auth, limits and logs as they are; Postgres rows so any replica serves any stream | Accepted |

**Going live**

| Option | Why it was tempting | Verdict |
|---|---|---|
| Relay when the stream is created | No second request | Rejected: egress gives up after 30 s; a broadcaster who takes a minute to set up would have a failed relay, and nothing tells us |
| Relay from LiveKit's `participant_joined` / `track_published` webhooks | No client step, and it works for fixed-token encoders | Deferred: a signed-webhook receiver is its own work (ADR-0050, ADR-0053); this decision leaves room for it, as the go-live step is idempotent |
| Relay from a periodic sweep | No client step | Rejected: a sweep cannot tell a publisher still setting up from one that is gone, and each blind attempt holds egress for its 30 s |
| `POST /api/v1/live/{id}/start` after the WHIP POST's `201`: the service starts the packager, waits for it to listen, relays, and answers `live` | One request at a known moment, idempotent, with an answer the client can show ("you are live") | Accepted |

**Starting the packager**

| Option | Why it was tempting | Verdict |
|---|---|---|
| A controller watching a CRD or the rows, making Jobs | The gateway holds no Kubernetes credential | Rejected for now: another Deployment, image and protocol for one create per stream; the Role below gives the gateway exactly that create and nothing more. Reopen if a second component needs to start packagers |
| The gateway creates the Job and its Secret through the API server with its own service account, in a namespace that holds packagers only, under an admission policy | Two calls per stream on the reactor through libcurl, as LiveKit's are; a Role there with create on Jobs and Secrets and get on Jobs; the policy holds what it creates to a packager's shape | Accepted |
| The Job's spec built in C++ | No file to ship | Rejected: the by-hand path of RUNBOOK step 9 would drift from the gateway's; ops could not change a request or limit without a rebuild of the code |
| The template file (`deploy/askedin/live-packager/job.yaml`) baked into the gateway's image and filled with exactly its four variables | One template for the gateway and the runbook; it ships with the code that fills it | Accepted |
| A packager per stream as a child process of the gateway | No cluster needed | Accepted for development, the local stack and the browser suite only, behind the same port (`IPackagers`); one stream at a time, as every child takes the one ingest port the relay is configured with |

**Seeing the end**

| Option | Why it was tempting | Verdict |
|---|---|---|
| LiveKit webhooks (`participant_left`, `egress_ended`) | Exact | Deferred with the receiver above |
| The packager's state (the Job's status, the child's exit), looked at by a sweep every 10 s, and the playlist's `EXT-X-ENDLIST` whenever a status request finds it through the live cache | Both already exist and say the truth: the ENDLIST within a second of the end, the exit after the recording | Accepted |
| A packager that waits for its caller forever | What it did | Rejected: a relay that never arrives would hold a pod for 13 hours. The packager gains `ULW_LIVE_CALLER_WAIT_SECONDS`; the template sets 60, and a packager nobody calls ends its stream as SIGUSR1 would, recording whatever an earlier run published |

## Decision

- **Rows.** `live_streams` (migration 0011): id (a UUIDv7, whose text is a DNS label, so it names
  the Job, the pod, the playlist prefix, the live chat and `live_recordings`' key), owner, state
  (`starting`, `live`, `ended`; forward only), the SRT passphrase (48 random hex characters, never
  sent to a client or logged), created, live and end times, and why it ended (`owner`,
  `finished`, `failed`, `timeout`). A partial unique index keeps **one unfinished stream per
  user**; the insert refuses a stream past `ULW_LIVE_MAX_STREAMS` unfinished on the platform
  (what egress can relay: stage 1, prod 2, and at most 64, what one sweep reads) and a stream
  past `ULW_LIVE_STREAMS_PER_USER_PER_HOUR` (default 6) created by its owner in the hour before,
  whatever became of them (`429`): what a client looping on create and end costs is bounded.
  Creates run in one transaction that first takes a transaction-scoped advisory lock, in a
  statement of its own: the insert's counts then read a snapshot taken after the previous
  creator committed (a lock taken inside the insert's own statement would come after that
  statement's snapshot), so concurrent creates never pass a cap together. The same transaction
  opens the stream's live chat with the chat store's own guarded statement (ADR-0070's
  `record_live`), and ending a stream closes its chat to new joins in the transaction that ends
  it (`chat_rooms.closed_at`, migration 0012: the room keeps its kind and reads as closed with no
  members, so a join is `not_live`). Sockets already joined stay until they leave.
- **Who may broadcast.** Any signed-in user, unless `ULW_LIVE_BROADCASTER_CLAIM` names a claim
  and value; then only a token carrying it may start a stream (`403` otherwise). Watching stays
  open to any signed-in user (ADR-0059).
- **API** (docs/integration/live.md): `POST /api/v1/live` (a new stream with its first publisher
  ticket, `201`; or the owner's unfinished one with a fresh ticket, `200`), `POST
  /api/v1/live/{id}/ticket` (a fresh ticket), `POST /api/v1/live/{id}/start` (go live), `POST
  /api/v1/live/{id}/end`, and `GET /api/v1/live/{id}` (the status, for any signed-in user; the
  owner also sees `video_id`). Requests carry no body; the owner is the token's user, and
  someone else's stream answers `404`, as a video does. An ended stream answers `409`; the
  platform's cap and every dependency outage `503` with `Retry-After`; anything refused as made
  `500`. Every route counts against the user's request rate like the rest of the API.
- **Tickets** come from `IMediaRoom::join(owner, stream id as device, Publisher)` on generation 1
  of the stream's room, and only while the row has not ended. A stream never moves to a new
  generation: it ends.
- **Going live** starts the packager (idempotent), waits up to 30 s for it to listen (a Job's
  `status.ready`, polled every 0.5 s), calls `relay` with the stream's passphrase and a keyframe
  interval of the segment length (`ULW_LIVE_SEGMENT_SECONDS`, the template's 2 s), then marks the
  row live. A packager found finished or failed ends the stream (`409`). The relay is
  idempotent (ADR-0053), so a retried `start` is answered from the running relay.
- **Ending.** The owner's `end` ends the row, then closes the room's generation: the publisher
  goes, its egress ends, the SRT session closes, and the packager writes `EXT-X-ENDLIST`,
  records the stream (ADR-0055) and exits. A close that fails is `503`; the row has ended
  already, so no ticket is issued again, and a repeated `end` closes the room again. The other
  ends: a status request that finds the playlist ended ends the row (`finished`); a sweep every
  10 s ends a `starting` stream past `ULW_LIVE_START_WINDOW_SECONDS` (default 2 minutes) and a
  `live` one past 13 hours (`timeout`), and asks each live stream's packager how it stands:
  finished ends the row `finished`, failed or gone `failed`. Every gateway replica sweeps; every
  write is conditional, so they agree. Viewers polling a stream that just ended report one end
  per process, not one each. An end that lands while `start` is relaying is honoured: `start`
  finds the row ended, closes the room again (the relay just started ends with it) and answers
  `409`.
- **Packagers on the cluster.** They run in a namespace of their own (`apps-stage-live`,
  `apps-live`): a quota (running pods to the streams the platform takes, a day's Jobs and
  Secrets), a default-deny NetworkPolicy beside the packager's own (SRT in from egress in the
  gateway's namespace; DNS, Postgres and the object store out), and Pod Security enforced at
  `baseline` and warned at `restricted`. A packager meets every rule of `restricted` but one:
  its sandbox's `procMount: Unmasked` (ADR-0032), which `restricted` refuses for any pod and
  `baseline` admits only in a pod's own user namespace (`hostUsers: false`), checked with a
  server-side dry run against kube-apiserver v1.37.0. The gateway's service account
  (`video-gateway`, in the gateway's namespace) has a Role there and nowhere else: `create` and
  `get` on Jobs, `create` on Secrets; no `list`, `update`, `patch`, `delete`, `exec`, and no read
  of any Secret. A ValidatingAdmissionPolicy bound to that account in that namespace admits only
  a packager Job of the template's shape (labelled `live-packager`, instance its own name, one
  container of the packager's image, no init or ephemeral containers, emptyDir volumes only, no
  token, the default account, a user namespace, none of the node's namespaces, no privilege or
  escalation, no `envFrom`, and Secret references to `live-packager-secrets` and its own
  stream's Secret only) and Secrets named `live-packager-<stream>` of type `Opaque`, changed on
  update in their metadata only; `deploy/local/check-live-admission.py` checks each rule with a
  server-side dry run. The gateway creates the Job from the template in its image
  (`/usr/local/share/ulw/live-packager-job.yaml`, filled with the namespace, the configured image
  tag, the stream id and the owner, quoted, and a user id outside its own alphabet refused), then
  the stream's Secret `live-packager-<id>` (the passphrase) with the Job as its owner from its
  creation, so the Job's removal a day after it finishes removes it and a Job that could not be
  made leaves no Secret. The pod waits for the Secret, which exists before its image is pulled.
  A start that finds either made already finishes the first one's work. The token is an hour's
  projected token of the account, mounted into the gateway's container alone (the pod's
  automount is off), read off the loop and again once a minute old; the API server's
  certificate is checked against the cluster CA. A packager counts as listening once its Job
  reports a ready pod (`status.ready`): it has no readiness probe, so ready is its container
  started, which binds the SRT listener at once; the relay's SRT caller retries its handshake for
  seconds.
- **Relays are deduplicated per process.** The media adapter queues relay calls for one
  participant behind the one in flight in that process (ADR-0053); two gateway replicas asked to
  start the same stream at once each list LiveKit's egresses first, and only a start that lands
  within the other's half second before LiveKit records it can start a second relay. The second
  is to the same packager, whose listener admits one caller; the extra egress gives up.
- **The packager waits a minute for its caller** (`ULW_LIVE_CALLER_WAIT_SECONDS=60` in the
  template, 0 and unlimited by default), then ends its stream as SIGUSR1 would. That bounds a
  relay that never came, and makes a packager restarted after a crash (the Job's `OnFailure`)
  end and record its stream instead of waiting 13 hours for a relay that went with the crash.

## Consequences

- A WHIP client makes one more request than ADR-0053 described: `start` after its POST. Encoders
  with one fixed token (OBS, `whipsink`) need the page that set them up to call it once the
  encoder shows it is connected; `end` from that page ends their stream at once, where their own
  DELETE is refused after a minute (ADR-0053).
- A packager drained by SIGTERM (a node drain) is not continued: the row stays `live` until the
  sweep finds the Job complete and ends it `finished`, and the stream's playlist has no
  `EXT-X-ENDLIST`, as ADR-0047's stale-stream rule describes. Continuing a stream on a new Job is
  not done; a broadcaster starts a new stream.
- The gateway holds a Kubernetes credential that can create Jobs. It reaches only the
  packagers' namespace, which holds nothing but packagers and their Secrets, and the admission
  policy holds what it creates there to a packager's shape: a compromised gateway can start a
  packager, under the quota, and nothing else. The policy and its binding are cluster-scoped, so
  whoever applies the overlays must be allowed to apply those.
- The gateway reaches LiveKit's server API (7880) and the API server; its NetworkPolicy gains
  both. The local stack and the browser suite use the process runtime, which runs one stream at
  a time and passes a packager only the gateway's store and database settings.
- One unfinished stream per user is the whole quota: a second device of the same user gets the
  same stream and its tickets, so its POST replaces the first device's session (ADR-0053's one
  source per stream).
- Up to one sweep (10 s) passes between a packager's exit and the row's end, unless a status
  request sees the playlist end first; viewers rely on the playlist's own `EXT-X-ENDLIST`
  anyway.
- Monitor `live_streams_created_total`, `live_streams_went_live_total`,
  `live_streams_ended_total{reason}` (a rise of `failed` or `timeout` is packagers or relays not
  working) and `live_dependency_failures_total{dependency}`.
- Reopen when a LiveKit webhook receiver exists (relay on `track_published`, end on
  `participant_left`), when streams must survive a packager's drain, or when a second component
  needs to start packagers.
