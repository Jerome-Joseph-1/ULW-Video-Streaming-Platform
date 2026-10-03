# 0053. Live ingest is WHIP straight to the SFU, relayed to the packager by its recorder

Status: Accepted, amended by 0088 (egress and Redis ship in `deploy/kubernetes/base`, sized by the operator)
Date: 2026-09-29
Amends: ADR-0050 (a closed generation is not closed to its publisher tickets; room kinds;
`IMediaRoom::relay`)

## Context

A broadcaster publishes a live stream with WHIP (RFC 9725, brief M30): POST an SDP offer and get
`201` with the answer and a `Location`, PATCH trickled candidates to that resource, DELETE it to
stop. RTMP is the brief's fallback. The media belongs to the SFU (ADR-0020), and ADR-0050
already gives a stream's source a publisher ticket: LiveKit's own WHIP endpoint
(`/whip/v1`, one-shot signalling) and a 60 s token that may publish camera and microphone into
one generation of one room and nothing else. The packager (ADR-0046) takes the stream over SRT
as MPEG-TS with H.264 and AAC, cut at keyframes every segment length T, from LiveKit's recorder
(egress). What M30 had to settle, read from LiveKit v1.13.7 (`pkg/service/whipservice.go`,
`pkg/service/roomallocator.go`, `pkg/rtc/transport.go`), egress v1.14.1
(`pkg/pipeline/source/sdk.go`, `pkg/config`) and protocol (`rpc/egress_client.go`), and checked
against the running servers:

- WHIP sends a bearer token with every request on the session, and LiveKit verifies it, expiry
  included (with a minute of skew), on each PATCH and DELETE as on the POST. It accepts any
  valid join token for the session's room and identity: there is no session secret, so the
  token is the only credential a session has.
- LiveKit's WHIP POST creates the room its token names without the `auto_create: false` check
  its `/rtc` path makes (`whipservice.go` never calls `ValidateCreateRoom`). A POST after
  `DeleteRoom` answered `201`, so a publisher ticket brings its closed generation's room back
  for as long as the ticket lasts. ADR-0050's "closing N leaves every credential naming N
  useless" holds for members only.
- A second session under one identity replaces the first (LiveKit disconnects it as a
  duplicate identity); two identities in one room are two producers.
- Egress has no automatic stream output: `CreateRoom`'s auto egress writes files and segments
  only. A participant egress (`StartParticipantEgress`) follows one identity, takes tracks as
  they are published, re-encodes (H.264 and AAC for SRT, keyframes at `key_frame_interval`),
  and ends when that participant leaves. It looks for the participant for 30 s, then fails.
  LiveKit generates each egress's id itself: a start request has no idempotency key.
- LiveKit's WHIP answers `OPTIONS` without ICE servers and puts them in the `201`'s `Link`
  headers, so a client behind STUNner (ADR-0037) learns its TURN servers after its POST.
- GStreamer's `whipsink` is gst-plugins-rs, which Ubuntu 24.04 does not package.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| LiveKit Ingress (a separate service) with its WHIP input | Stream keys that last, RTMP and WHIP behind one API, the fallback in the same box | Rejected: another deployment, and a second hop, in front of the same room a browser reaches directly; its stream-key model is a second credential scheme beside ADR-0050's tickets |
| A publisher ticket that lasts the longest stream (12 h), so one token serves the whole session | Every WHIP client works as it is, fixed-token encoders included | Rejected after review: with no session secret, whoever holds the token can PATCH or DELETE the live session, replace it with a POST, or bring the generation back after it was closed, for 12 h. A leaked or revoked ticket would outlive every remedy the stream service has |
| Short publisher tickets, and a fresh one from the stream service before every PATCH and DELETE | Nothing of ours between client and SFU; the stream service decides, per request, whether this stream is still the owner's current one (`join` refuses a closed handle) | Accepted |
| Our own WHIP resource that re-authorises each PATCH and DELETE and forwards it to LiveKit with a fresh token | Fixed-token encoders keep working; tokens never leave the server | Deferred to the stream service: a method-routed relay of three methods, headers (`Location`, `ETag`, `Link`, `If-Match`) and CORS needs the stream's ownership and state, which only that service will have |
| Start the relay from LiveKit's webhooks (`track_published`) | Nobody has to say the stream is live | Deferred: a signed-webhook receiver belongs to the stream service too; ADR-0050 already expects one for calls |
| Relay with room composite egress | One request per room, whoever publishes | Rejected: it renders the room in headless Chrome, twice a participant egress's cost by egress's own accounting (4 cores against 2), for one publisher |
| Relay with a participant egress to the packager's SRT listener | One identity, dynamic tracks, re-encoded to exactly what the packager copies, ends when the publisher leaves | Accepted |
| RTMP ingest now, through LiveKit Ingress | The brief's fallback | Deferred: clients are browsers (brief section 2), which cannot speak RTMP, and OBS (30+), GStreamer and FFmpeg (8.0+) speak WHIP. Reopen for an encoder without WHIP; it would be Ingress's RTMP input publishing as the stream's identity, so the relay below stays as it is |
| Test with gst `whipsink` built from its crate | The brief's acceptance names it | Accepted: `gst-plugin-webrtchttp` 0.13.5 by SHA-256, built against the system GStreamer 1.24 with a committed `Cargo.lock` and a pinned Rust release |

## Decision

- **Publishing.** A stream's source is a participant with the publisher ticket of ADR-0050. It
  POSTs its offer to the ticket's URL with the ticket as bearer token and uses LiveKit's
  answer, `Location` (relative to that URL), `ETag` and `Link` headers as RFC 9725 says. A client
  that trickles sends its offer before gathering, sets the `Link` ICE servers, then gathers and
  PATCHes (RFC 9725 sections 4.3.1 and 4.6); on Askedin's cluster that is how it reaches
  STUNner, since LiveKit offers only its pod address.
- **A publisher ticket lives 60 s, as a member's does**, and a client asks the stream service
  for a fresh one before every request after its POST: each PATCH, an ICE restart, the DELETE.
  The service issues it through the same `join` of the stream's current generation, so a stream
  it has ended or moved on cannot be touched with a ticket it gave out before. That bounds a
  leaked or revoked ticket, and the revival of a closed generation, to one minute plus
  LiveKit's minute of skew.
- **Fixed-token encoders** (OBS, `whipsink`) must POST within the ticket's minute; their own
  DELETE after it is refused (`401`), and the stream then ends when LiveKit drops the silent
  participant (17 s after a `whipsink` was killed, three local runs; ADR-0050 measured 20 to
  22 s for an SDK client). Two follow-ups wait for the stream service: a method-routed PATCH and
  DELETE re-authoriser at our own URL, so such encoders get a prompt end, and a sweep that
  deletes any generation's room LiveKit holds that is not the stream's current one, so a room a
  ticket brought back inside its minute does not linger.
- **One producer.** Every ticket for one stream names one identity: the stream's owner and the
  stream's own id as the device. A second POST under that identity (an encoder that
  reconnects, a second tab) replaces the first session; viewers never join the room
  (ADR-0014), and the recorder only subscribes. A DELETE of the replaced session's resource
  answers `200` and leaves the new session running (tested). The stream's room is opened as a
  `MediaRoomKind::Stream` without a participant limit, since the recorder joins beside the
  publisher.
- **A room's kind is fixed per handle by the caller that opens it**, and `join` enforces it: a
  call's room issues member tickets only and a stream's room publisher tickets only (`Refused`
  otherwise, before anything is sent). Otherwise a publisher ticket for a call's generation
  could bring back a generation that was closed to put someone out, and the expelled member
  could rejoin it. The adapter cannot check that the caller named the right kind; the stream
  service and the call handler each open only their own.
- **Relay.** `IMediaRoom::relay(user, device, MediaRelay{stream, passphrase, keyframe_interval})`
  starts a participant egress for that identity with one SRT output to the stream's packager
  (ADR-0046), re-encoded to 1280x720 at 30 fps and 2800 kbit/s (the worker's 720p rung) with a
  keyframe every `keyframe_interval`, the packager's T. Keyframes are therefore the recorder's,
  not the source's: no encoder setting is asked of a publisher.
  - **Where** is the adapter's configuration, not the caller's: `packager_srt`, `srt://host:port`
    with an optional `{stream}` in the host, to which the relay adds `streamid` and the
    percent-encoded `passphrase`. A caller cannot point the recorder anywhere else. With
    `{stream}` configured, a stream id must also be a DNS label (lowercase letters, digits and
    `-`, not at either end, at most 63): stream ids allow uppercase, `_` and 64 characters, and
    such an id is refused rather than rewritten, so two ids never name one host.
  - **Refused unsent**: a call's room, a stream id that is not one key segment, a passphrase SRT
    would refuse, a keyframe interval outside the packager's 2 to 10 s, an adapter with no
    packager configured, a room closed through its handle (`Closed`).
  - **Idempotent**: the relay lists the room's active egresses first (`ListEgress`) and answers
    the id of one starting or active for that identity instead of starting another, so a retry
    after a lost answer does not start a second. `active` in that listing also returns
    egresses that are ending, which carry nothing more; those do not count. LiveKit records an
    egress only once its start has answered, half a second later, so calls for the same
    identity while one is in flight are queued on its answer instead of listing for
    themselves (one reactor thread, no lock). A start that answers without an id is
    `Unavailable`; the retry's listing finds it. The one case left is a retry after our own
    start timed out while LiveKit may still be starting it (below). The start has its own limits: 5 s, ten times
    the 530 to 541 ms seven local starts took (500 ms of it LiveKit's RPC waiting for a busier
    recorder to bid, `ShortCircuitTimeout`); the listing reads up to 64 KiB, as each egress is
    about 3 KiB.
  - Both calls carry a 10 s token with `roomRecord` only. The caller relays once the
    publisher's POST has succeeded, because egress looks for the participant for 30 s only.
- **Ending.** DELETE (or LiveKit dropping a silent publisher, or closing the generation) removes
  the participant; its egress ends, which closes the SRT session, and the packager writes
  `EXT-X-ENDLIST` (ADR-0047). A publisher that reconnects under the same identity is not a clean
  continuation: in two trial runs of a second whipsink under a relayed stream, the recorder once
  went on relaying the new session and once the packager ended the stream as broken (a segment
  past its target duration at the switch). A source that must restart DELETEs first, and comes
  back as a new stream.
- **Deployment.** The stage HTTPRoute sends `/whip` to LiveKit beside `/rtc`, on every hostname of
  the gateway as the other routes. Egress (v1.14.1, pinned by digest) and the Redis LiveKit needs
  to reach it are in `deploy/local/compose.yaml` and the tests; they go to Askedin's overlays
  together with the packager's own, since a relay has nowhere to go until a packager is
  deployed.
- **Tests** (`tests/call/ingest.spec.mjs`, run by `tests/call/run.sh`; CI job `ingest`):
  - a `gst-launch-1.0 … whipsink` publisher is exactly one producer with one audio and one video
    track, and still one after a second session with the same ticket;
  - SIGINT makes whipsink DELETE (`200`), and the participant is gone within 5 s where LiveKit
    takes 17 s or more to drop a silent one;
  - a DELETE with the POST's ticket two minutes past its expiry is `401`, one with a fresh
    ticket is `200` and ends the session while the source keeps sending;
  - a replaced session's DELETE leaves the session that replaced it;
  - a browser posts an offer with no candidate, drops the answer's, and PATCHes its own with a
    fresh ticket after the session exists, and connects through a peer-reflexive pair on a
    trickled candidate's socket, which only LiveKit's checks towards that candidate can
    produce;
  - a whipsink stream, relayed (and relayed again, which answers the same id), becomes a
    packager playlist of 2 s segments that ends with `EXT-X-ENDLIST` after the DELETE;
  - a probe pins the revival: a publisher ticket's POST after `DeleteRoom` gets `201`, so an
    upgrade of LiveKit that adds the check is noticed.

  Local LiveKit, Redis and egress run on the host network: egress joins rooms as a client at the
  loopback address LiveKit advertises, and LiveKit's checks towards a publisher's host
  candidates must reach the host.

## Consequences

- A client needs the stream service for every WHIP request, not only the first. A client that
  loses its way to the service mid-stream cannot DELETE; its stream ends by the
  silent-participant drop once it stops sending.
- A publisher ticket, leaked, can for its minute publish into its generation as the stream's
  identity, which replaces the real source and may be what the recorder relays next, or bring
  a closed generation's room back. Nothing downstream consumes such a room: no member ticket
  names it again, viewers never join rooms, and nothing relays it. The sweep above removes it.
- The relay's passphrase travels to LiveKit in the request, and LiveKit hands it back in every
  `EgressInfo` (`ListEgress`, egress webhooks) and logs it in egress's request log, since SRT
  URLs are not redacted. Our code never logs it (the call suite's harness wrapper hides it in
  its errors). Whoever reads LiveKit's API or logs can feed that stream's packager; the
  packager's listener still admits only one caller per run.
- A start that LiveKit completes after our 5 s timeout can still race an immediate retry's
  listing; the stream service should retry after the start's own RPC deadline (10 s,
  `rpc/egress_client.go`) rather than at once.
- Measured locally (four shared cores, `docker stats` over a 9 s relay of a 640x360 source): the
  recorder takes 0.5 to 1 core and 300 MB per stream, for its 720p re-encode. The playlist had
  three segments 7.6 to 7.9 s after `relay` (the recorder joining, subscribing and encoding,
  then three 2 s segments), and `EXT-X-ENDLIST` 0.3 to 0.4 s after the DELETE.
- Egress admits a request only while its CPU cost is idle (`participant_cpu_cost`, 2 cores by
  default); the local compose file lowers it so a busy machine does not refuse. Askedin's egress
  must be sized from the measurement above and keep the default, or a busy node refuses
  streams (`Unavailable` from `relay`).
- The relay's failures after the request is accepted (participant never came, SRT refused) are
  seen only in egress's own status and webhooks; until the stream service reads those, a
  packager whose caller never arrives waits.
- Monitor egress's active and failed counts, its CPU per stream, WHIP `401`s (clients not
  refreshing tickets, or fixed-token encoders ending late), duplicate-identity disconnects in
  streams (reconnecting encoders), and rooms whose generation is not current.
- Reopen when the stream service exists (the re-authoriser and the sweep above), if LiveKit adds
  the room check to WHIP or a session secret, if an RTMP-only encoder must be supported, or if
  LiveKit gains a stream output for automatic egress.
