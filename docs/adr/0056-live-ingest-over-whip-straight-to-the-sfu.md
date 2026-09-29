# 0056. Live ingest is WHIP straight to the SFU, relayed to the packager by its recorder

Status: Accepted
Date: 2026-09-29
Amends: ADR-0050's publisher ticket (its lifetime), and adds `IMediaRoom::relay` to its port

## Context

A broadcaster publishes a live stream with WHIP (RFC 9725, brief M30): POST an SDP offer and get
`201` with the answer and a `Location`, PATCH trickled candidates to that resource, DELETE it to
stop. RTMP is the brief's fallback. The media belongs to the SFU (ADR-0020), and ADR-0050
already gives a stream's source a publisher ticket: LiveKit's own WHIP endpoint
(`/whip/v1`, one-shot signalling) and a token that may publish camera and microphone into one
generation of one room and nothing else. The packager (ADR-0046) takes the stream over SRT as
MPEG-TS with H.264 and AAC, cut at keyframes every segment length T, from LiveKit's recorder
(egress). What M30 still had to settle, read from LiveKit v1.13.7 (`pkg/service/whipservice.go`,
`pkg/rtc/transport.go`) and egress v1.14.1 (`pkg/pipeline/source/sdk.go`, `pkg/config`):

- WHIP sends the same bearer token with every request on the session, and LiveKit verifies it,
  expiry included, on the PATCH and the DELETE as on the POST. The publisher ticket lived 60 s.
- A second session under one identity replaces the first (LiveKit disconnects it as a
  duplicate identity); two identities in one room are two producers.
- Egress has no automatic stream output: `CreateRoom`'s auto egress writes files and segments
  only. A participant egress (`StartParticipantEgress`) follows one identity, takes tracks as
  they are published, re-encodes (H.264 and AAC for SRT, keyframes at `key_frame_interval`),
  and ends when that participant leaves. It looks for the participant for 30 s, then fails.
- LiveKit's WHIP answers `OPTIONS` without ICE servers and puts them in the `201`'s `Link`
  headers, so a client behind STUNner (ADR-0037) learns its TURN servers after its POST.
- GStreamer's `whipsink` is gst-plugins-rs, which Ubuntu 24.04 does not package.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| LiveKit Ingress (a separate service) with its WHIP input | Stream keys that last, RTMP and WHIP behind one API, the fallback in the same box | Rejected: another deployment, and a second hop, in front of the same room a browser reaches directly; its stream-key model is a second credential scheme beside ADR-0050's tickets |
| Our own WHIP handler that terminates the resource and relays each request to LiveKit with a fresh short token | Short-lived LiveKit tokens; our code sees the session start, so it could start the recorder itself | Rejected for now: an HTTP relay of three methods, headers (`Location`, `ETag`, `Link`, `If-Match`) and CORS, a second hop on every request, and a credential of our own that must last the stream anyway, for no media function; the SFU owns the media plane (ADR-0020). Reopen if tickets must be revocable per session rather than per generation |
| The publisher ticket, straight to LiveKit's WHIP endpoint, living as long as the longest stream | Nothing new between client and SFU; the ticket already exists | Accepted |
| Start the relay from LiveKit's webhooks (`track_published`) | Nobody has to say the stream is live | Deferred: a signed-webhook receiver belongs to the service that owns streams, which does not exist yet; ADR-0050 already expects one for calls |
| Relay with room composite egress | One request per room, whoever publishes | Rejected: it renders the room in headless Chrome, twice a participant egress's cost by egress's own accounting (4 cores against 2), for one publisher |
| Relay with a participant egress to the packager's SRT listener | One identity, dynamic tracks, re-encoded to exactly what the packager copies, ends when the publisher leaves | Accepted |
| RTMP ingest now, through LiveKit Ingress | The brief's fallback | Deferred: clients are browsers (brief section 2), which cannot speak RTMP, and OBS (30+), GStreamer and FFmpeg (8.0+) speak WHIP. Reopen for an encoder without WHIP; it would be Ingress's RTMP input publishing as the stream's identity, so the relay below stays as it is |
| Test with gst `whipsink` built from its crate | The brief's acceptance names it | Accepted: `gst-plugin-webrtchttp` 0.13.5 by SHA-256, built against the system GStreamer 1.24 with a committed `Cargo.lock` |

## Decision

- **Publishing.** A stream's source is a participant with the publisher ticket of ADR-0050. It
  POSTs its offer to the ticket's URL with the ticket as bearer token and uses LiveKit's
  answer, `Location` (relative to that URL), `ETag` and `Link` headers as RFC 9725 says. Nothing
  of ours is between client and SFU. A client that trickles sends its offer before gathering,
  sets the `Link` ICE servers, then gathers and PATCHes (RFC 9725 sections 4.3.1 and 4.6); on
  Askedin's cluster that is how it reaches STUNner, since LiveKit offers only its pod address.
- **A publisher ticket lives 12 h plus the 60 s connect window** (`kPublisherTicketTtl`), the
  longest stream the packager takes (ADR-0047) begun at the end of the window, so its DELETE
  still passes. The window to POST is still a member's: LiveKit drops an empty room after 60 s.
  Taking it back is what ADR-0050 already does: close the generation.
- **One producer.** Every ticket for one stream names one identity: the stream's owner and the
  stream's own id as the device. A second POST under that identity (an encoder that
  reconnects, a second tab) replaces the first session; viewers never join the room
  (ADR-0014), and the recorder only subscribes. The stream's room is opened without a
  participant limit, since the recorder joins beside the publisher.
- **Relay.** `IMediaRoom::relay(user, device, MediaRelay{url, keyframe_interval})` starts a
  participant egress for that identity with one SRT output (`srt://<packager>:<port>?streamid=
  <stream>&passphrase=<passphrase>`, ADR-0046), re-encoded to 1280x720 at 30 fps and
  2800 kbit/s (the worker's 720p rung) with a keyframe every `keyframe_interval`, the
  packager's T. Keyframes are therefore the recorder's, not the source's: no encoder setting is
  asked of a publisher. The call is authorised with a 10 s token carrying `roomRecord` only. A
  target that is not `srt://` is refused unsent; a room closed through its handle refuses as
  for `join`. The caller relays once the publisher's POST has succeeded (the client says so, or
  a webhook does when the stream service has them), because egress looks for the participant
  for 30 s only.
- **Ending.** DELETE (or LiveKit dropping a silent publisher, or closing the generation) removes
  the participant; its egress ends, which closes the SRT session, and the packager writes
  `EXT-X-ENDLIST` (ADR-0047). A publisher that reconnects under the same identity is not a clean
  continuation: in two trial runs of a second whipsink under a relayed stream, the recorder once
  went on relaying the new session and once the packager ended the stream as broken (a segment
  past its target duration at the switch). A source that must restart DELETEs first, and comes
  back as a new stream.
- **Deployment.** The stage HTTPRoute sends `/whip` to LiveKit beside `/rtc`. Egress (v1.14.1,
  pinned by digest) and the Redis LiveKit needs to reach it are in `deploy/local/compose.yaml`
  and the tests; they go to Askedin's overlays together with the packager's own, since a relay
  has nowhere to go until a packager is deployed.
- **Tests** (`tests/call/ingest.spec.mjs`, run by `tests/call/run.sh`): a `gst-launch-1.0 …
  whipsink` publisher is exactly one producer with one audio and one video track, and still one
  after a second session with the same ticket; SIGINT makes whipsink DELETE (`200`), and the
  participant is gone within 5 s, where LiveKit takes 20 s or more to drop a silent one; a
  browser posts an offer with no candidate, drops the answer's, and PATCHes its own after the
  session exists, and connects
  through a peer-reflexive pair on a trickled candidate's socket, which only LiveKit's checks
  towards that candidate can produce; and a whipsink stream, relayed, becomes a packager playlist
  of 2 s segments that ends with `EXT-X-ENDLIST` after the DELETE. Local LiveKit, Redis and egress
  run on the host network: egress joins rooms as a client at the loopback address LiveKit
  advertises, and LiveKit's checks towards a publisher's host candidates must reach the host.

## Consequences

- A publisher ticket is a bearer credential for 12 h, like any WHIP token or stream key; leaked,
  it can publish into that generation as the stream's identity, which replaces the real source
  and, as above, may be what the recorder relays next. Moving the stream to a new generation
  (ADR-0050) and relaying from there is the remedy, and the stream service must offer it.
- Closing a generation does not stop its publisher tickets from opening it again: LiveKit
  v1.13.7's WHIP endpoint creates the room a token names without the `auto_create: false` check
  its `/rtc` path makes (`whipservice.go` never calls `ValidateCreateRoom`; a POST after
  `DeleteRoom` answered `201` here). The room it brings back is an orphan: no member ticket names
  it again, viewers never join rooms, and nothing relays it to a packager, so the holder can
  spend the SFU's resources and reach no one. A 60 s ticket kept that window short; 12 h is the
  price of a DELETE that works. Watch LiveKit's releases for the check, and reopen the relay
  option above if orphan rooms show up in its room count.
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
- Monitor egress's active and failed counts, its CPU per stream, WHIP `401`s after long
  sessions (a ticket that was not a publisher ticket), and duplicate-identity disconnects in
  streams (reconnecting encoders).
- Reopen if tickets must be withdrawn per session, if an RTMP-only encoder must be supported, or
  if LiveKit gains a stream output for automatic egress.
