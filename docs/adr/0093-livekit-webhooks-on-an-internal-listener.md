# 0093. LiveKit's webhooks take a stream live and end it, on a listener of the gateway's own that only LiveKit reaches

Status: Accepted
Date: 2026-10-03
Amends: ADR-0092 (what takes a stream live, and what ends it)

## Context

ADR-0092 takes a stream live on its owner's word: the broadcaster's client calls
`POST /api/v1/live/{id}/start` after its WHIP POST succeeds, and `end` to finish. It leaves a
LiveKit webhook receiver as the follow-up, for two cases the client's word does not cover:

- **Fixed-token encoders** (OBS, `whipsink`) publish with no page beside them to call `start`.
- **Clients that crash**, or lose their network, never call `end`. The relay ends when LiveKit
  drops the publisher, the packager ends the playlist and the sweep ends the row (`finished`) once
  the packager has exited, minutes later; a stream that never went live waits out its 2-minute
  start window.

What LiveKit v1.13.7 does, from its source (`webhook/` and `auth/` of the protocol module it pins,
`pkg/service/wire.go`):

- **Delivery.** For each URL in `webhook.urls`, one queue per resource (the room's name for room
  and participant events): events for one room go out one at a time, in order, each waiting for
  the previous one's answer. A send is retried by `go-retryablehttp`'s defaults (4 retries,
  1 to 30 s apart, on a connection error, `429` or a `5xx`); an event that waited in its queue
  longer than 30 s is dropped. So an event can arrive late, twice (a retry after an answer that
  was lost), or not at all, and with several gateway replicas behind one Service, consecutive
  events for a room can reach different replicas, which act on them in any order.
- **Format.** `POST`, `Content-Type: application/webhook+json`, the body a `livekit.WebhookEvent`
  as protojson (lowerCamelCase, int64 as strings, unset fields omitted).
- **Auth.** `Authorization` carries a bare JWT (no `Bearer`), HS256, issued (`iss`) by the API key
  named in `webhook.api_key` and signed with its secret, with `iat`, `nbf`, `exp` five minutes
  later, and `sha256`: the standard base64 of the body's SHA-256. LiveKit's own receiver checks
  the signature, the issuer, the expiry (required, 60 s leeway) and the body hash, the last in
  constant time.
- **Events.** `participant_joined`, `track_published` (once per track: a WHIP publisher has two),
  `participant_left`, `participant_connection_aborted`, `room_finished`, and others. A
  participant's `sid` is its session; a full reconnect leaves with one and joins with another.
  LiveKit's recorder joins a relayed room as a participant of its own.

From the stream service as it stands: every stream's room is `<stream id>:1` and its publisher's
identity `<owner>/<stream id>` (ADR-0092); going live is idempotent and safe to repeat; every end
is a conditional write, so the first reason stands.

## Options

**Where the endpoint is served**

| Option | Why it was tempting | Verdict |
|---|---|---|
| A route on the public listener under `/api/v1/live` | No new port, Service or policy | Rejected: the public HTTPRoute sends `/api/v1/live` to the gateway, so anyone on the internet could post to it; the token keeps them out, but nothing needs it reachable, and every forged request would cost an HMAC and a log line on the public path |
| A route on the public listener outside the routed prefixes (`/internal/...`) | Unreachable through Envoy as the routes stand | Rejected: one widened `PathPrefix` later and it is public again, silently; and the public listener's limits (per-user rate, CSRF, cookie rules) do not fit a server-to-server caller |
| A listener of its own on another port, behind a ClusterIP Service no route names, admitted by NetworkPolicy from LiveKit's pods only | Unreachable from the internet by construction; its own small limits; off unless configured | Accepted |

**Believing a webhook**

| Option | Why it was tempting | Verdict |
|---|---|---|
| LiveKit's own scheme: HS256 JWT with the body's hash, checked as `webhook.Receive` does | What LiveKit sends; the key pair the gateway already holds | Accepted, with the signature compared before any claim is read, `alg` pinned to HS256, `exp` required, and the hash and the MAC compared in constant time |
| Network position alone (NetworkPolicy) | Simpler | Rejected: a policy is one mistake from open, and any pod in LiveKit's namespace could post |
| Remembering event ids to refuse replays | Exact | Rejected: within a token's five minutes a replay can only repeat an idempotent start or ask for a check whose outcome LiveKit decides (below), and ids would be one more table per replica |

**What an event does**

| Option | Why it was tempting | Verdict |
|---|---|---|
| End the stream on `participant_left` at once | Simplest | Rejected: a full reconnect is a leave and a join seconds apart, and the events can arrive in either order or at different replicas |
| Track presence per replica and end on its own count | No extra call | Rejected: a replica that missed the join (sent to another) would end a stream whose publisher is back |
| After a grace, ask LiveKit whether the publisher is connected (`GetParticipant`, which does not recreate a dropped room) and end only if not | LiveKit is the one that knows; late, duplicated or misrouted events cannot end a stream whose publisher is there | Accepted |
| Go live on `participant_joined` and `track_published` through the owner's start path | One path, already idempotent, already safe against a concurrent `start` | Accepted |

## Decision

- **Listener.** With `ULW_LIVE_WEBHOOK_PORT` set (it must differ from `ULW_LISTEN_PORT`; live
  streams must be on), the gateway binds a second plain-HTTP listener serving exactly
  `POST /livekit/webhook`; anything else is `404` or `405`. At most 32 connections, a 10 s idle
  timeout, a 64 KiB body cap (`413` from the `Content-Length` alone, before the body is read), and
  one token bucket for the listener (200 at once, 100 a second; `429` with `Retry-After`, which
  LiveKit retries). On stage: port 8081, Service `video-gateway-hooks` (ClusterIP, named by no
  route), and NetworkPolicy `video-gateway-hooks` admitting only LiveKit's pods to that port.
  LiveKit posts to `http://video-gateway-hooks.<namespace>.svc.cluster.local:8081/livekit/webhook`
  with `webhook.api_key` the key it already shares with the gateway (`sfu-secrets`).
- **Verification**, before the body is parsed: a bare HS256 JWT of three base64url parts, at most
  4 KiB; the HMAC-SHA256 under `LIVEKIT_API_SECRET`, compared with `CRYPTO_memcmp`; then `iss`
  equal to `LIVEKIT_API_KEY`, `exp` present and not more than 60 s past, `nbf` not more than 60 s
  ahead, and `sha256` equal (constant time) to the body's. Any failure is `401` and the body is
  not read as an event; a verified body that is not an event is `400`. A verified event is
  answered `200` before it is acted on, so LiveKit's next event for the room never waits on the
  stream service and is never dropped for age because of it.
- **Mapping.** Only rooms named `<stream id>:1` and participants named `<owner>/<stream id>` count;
  a call's room, LiveKit's recorder and every other event are acknowledged and ignored.
  - `participant_joined` or `track_published` for a session not seen leaving: the stream's go-live
    (`LiveStreams::go_live` with the identity's owner), the path `POST .../start` takes, one at a
    time per stream and replica; repeats for a session already live ask nothing again. A start a
    dependency could not answer is retried every 2 s while the publisher stays, 5 times. Not the
    owner's stream, or no such stream: nothing, and the stream is not followed.
  - `participant_left` or `participant_connection_aborted` leaving no session present, or
    `room_finished`: a grace of `ULW_LIVE_PUBLISHER_GRACE_SECONDS` (10 s, 1 to 300) on the
    reactor's timer. A join of a new session within it cancels it. At its end (after any start
    under way has answered) the service reads the row: only a `live` stream is considered (a
    `starting` one keeps its start window, since its room comes and goes with its tickets while an
    encoder is set up); it asks LiveKit whether the publisher is connected, and ends the stream
    only if not, with the new reason `publisher_left` (`live_streams.end_reason`, migration 0013),
    then closes its room. A check LiveKit could not answer is retried every 2 s, 5 times, and then
    left to the sweep.
  - A session seen leaving is remembered (16 per stream), so its late join or track changes
    nothing. Each replica follows at most 1024 streams; an idle one makes room for a new one.
- **The owner's API stays** as ADR-0092 has it. `start` and the webhook's start run the same
  idempotent path, in either order or at once (one packager, one relay); `end` and
  `publisher_left` are both conditional ends, and whichever is first stands. Clients may keep
  calling both; fixed-token encoders and crashed clients no longer need to.
- **Prod** keeps live off (ADR-0092): its overlay ships the Service and NetworkPolicy, which admit
  nothing until the gateway listens on the port; the gateway's `ULW_LIVE_WEBHOOK_PORT` and
  LiveKit's `webhook` block are added with the rest of live at phase-6 (RUNBOOK step 9).

## Consequences

- A stream goes live within a second or two of its publisher's first track, even if nobody calls
  `start`, and ends `publisher_left` about the grace after its publisher is gone, instead of
  `finished` minutes later. A status request that sees the playlist ended first still ends it
  `finished`, as before; both are a departed publisher.
- The gateway trusts LiveKit's key pair to say who publishes; whoever holds that secret can
  already mint publisher tickets, so this adds no new party to trust.
- A replay within a token's five minutes can repeat a start (idempotent) or a departure, whose end
  LiveKit's own answer decides. Event ids are not remembered.
- Each departure costs a row read and one `GetParticipant`; each ending, the room's deletion as
  before. LiveKit's room events for calls cost a JSON parse and a counter.
- `live_webhooks_total{outcome}`, `live_webhook_refusals_total{reason}` (a rise of `signature`
  or `unknown_key` is a key mismatch between LiveKit and the gateway, or someone posting),
  `live_publisher_departures_total`, `live_publisher_returns_total`, `live_publisher_kept_total`
  and `live_streams_ended_total{reason="publisher_left"}` show it working.
- The local stack's LiveKit (deploy/local/compose.yaml) posts to `127.0.0.1:7890`; a gateway
  started with `ULW_LIVE_WEBHOOK_PORT=7890` receives them, and while none listens LiveKit logs
  failed sends and drops them, which changes nothing else.
- Reopen if LiveKit gains webhook signing with a key of its own, if streams must survive a
  publisher's reconnect on the same packager, or if the start should move off `track_published`
  to the relay's own `egress_started`.
