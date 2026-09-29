# 0033. Clients reach the SFU with a ticket, not through our signalling

Status: Accepted
Date: 2026-09-29
Amends: the media port sketched in the brief (section 8.15), `IMediaRoom::join(UserId, DeviceId,
IceCredentials) -> IMediaParticipant { apply_offer(sdp) -> answer, add_ice_candidate, close }`

## Context

ADR-0020 puts LiveKit behind `IMediaRoom`/`IMediaParticipant`. The sketched port has the server
apply a client's SDP offer and return the SFU's answer, with trickled candidates carried over the
room WebSocket as `webrtc_offer`, `webrtc_answer` and `ice_candidate`.

LiveKit does not negotiate that way. A LiveKit client holds two peer connections: it offers on
the publisher connection, and the server offers on the subscriber connection, renegotiating it
every time a track is added or removed. Every one of those exchanges, plus join, leave, mute,
track settings, token refresh and connection-quality reports, travels as protobuf over LiveKit's
own WebSocket (`/rtc`), which the server also uses to detect a dead client (ping every 5 s,
timeout 15 s in v1.13.7). The one server API that takes an offer and returns an answer is the
WHIP endpoint, built for ingest (option c).

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| (a) Our server opens LiveKit's `/rtc` WebSocket for each participant and relays between it and the room WebSocket | Keeps `apply_offer -> answer` literally true; clients never learn LiveKit exists | Rejected: the port would still be a lie, because half the offers come from the SFU (subscriber side), so `apply_offer` models one direction of two. The server would implement a WebSocket client and LiveKit's protobuf signalling, and track its protocol versions, for no function of its own. Every renegotiation and ping would cross an extra hop and a second WebSocket per participant, and LiveKit's dead-peer detection would see our relay, not the client |
| (b) Our server authorises the call, opens the LiveKit room and hands the client a ticket (URL and a short-lived access token for one identity in one room); the browser SDK talks to LiveKit directly | What LiveKit's own clients, STUNner's LiveKit integration and every LiveKit deployment do; nothing of LiveKit's protocol in our code; failure detection stays end to end | Accepted |
| (c) Our server drives LiveKit's built-in WHIP endpoint (`/whip/v1`, "one-shot signalling"): POST the client's offer, return the answer, PATCH trickled candidates, DELETE on close | Real offer/answer over plain HTTP, exactly the sketched port's `apply_offer`, `add_ice_candidate` and `close`; no protobuf | Rejected, from LiveKit v1.13.7's source (`pkg/service/whipservice.go`, `roommanager_service.go`, `pkg/rtc/participant.go`). A one-shot session is never renegotiated, so a client receives only the tracks named in its offer, and the answer is held until those tracks are subscribed. In a 1:1 call each side would wait for the other's tracks, which exist only once the other side has its answer: the first to join needs a second, receive-only session once the peer publishes, ordered by events we would have to learn from webhooks. Video subscription in this mode is marked unfinished in the source (no congestion control for it). One-shot clients hear nothing in band: no participant left, mute or quality events, and no resume, so every one of those becomes our protocol to design |

## Decision

- The port is `ISfu::open_room(RoomId, max_participants) -> IMediaRoom`,
  `IMediaRoom::join(UserId, DeviceId) -> IMediaParticipant`, `IMediaRoom::close()`, and
  `IMediaParticipant { ticket(), remove() }` (`core/include/core/ports/media.hpp`). A
  `MediaTicket` is an endpoint, an opaque credential and its expiry; core never sees a LiveKit
  word. `apply_offer`, `add_ice_candidate` and the SDP they carry are gone from the port.
- Signalling over the room WebSocket is: the client asks to join a call; the call handler checks
  membership (the JWKS-authenticated user, ADR-0018), opens the room (idempotent) on the owning
  node, and answers with the ticket. Offers, answers and candidates run between browser and
  LiveKit. `codec/sdp` stays useful as test tooling and to validate SDP captured from LiveKit
  sessions, not as a relay.
- `infra/sfu/livekit` implements the port. Rooms are LiveKit rooms named by the room id, created
  with `max_participants` and `empty_timeout`; a participant is the identity `<user>/<device>`,
  so two devices of one user are two participants. Tokens are HS256 JWTs with LiveKit's `video`
  grant, minted in process from `LIVEKIT_API_KEY`/`LIVEKIT_API_SECRET`: a join token grants
  `roomJoin`, publish and subscribe in one room and nothing else, for 6 minutes (LiveKit pushes a
  fresh token to a connected client every 5 minutes, and a resume before the first refresh
  presents the ticket); each RoomService call carries its own 10 s token with only the
  permission it needs. RoomService (Twirp JSON over HTTP) runs on the reactor through
  `infra/curl`.
- Removal is idempotent: removing a participant or closing a room that LiveKit has already
  dropped succeeds. LiveKit's `auto_create` is off, so a ticket cannot recreate a room with
  default settings.
- TURN (STUNner, ADR-0013) is not an SFU concern: the call handler sends the per-session TURN
  credentials next to the ticket, and the client hands them to the SDK as its ICE servers with a
  relay-only policy. That is why `IceCredentials` left `join`.
- The browser loads `livekit-client`, pinned to an exact version by lockfile (2.22.3 in the call
  suite).

## Consequences

- Clients carry a LiveKit SDK; the ticket is the only LiveKit-shaped thing we send them.
  Replacing the SFU changes the client SDK, the adapter and the ticket's contents, but not the
  port or the room WebSocket.
- A ticket admits its holder until it expires even after `remove()`. Withholding new tickets is
  what ends access; if that window matters, shorten the ticket and let LiveKit's refresh carry
  connected clients, or add the participant to a deny list the adapter consults.
- Our server no longer sees media negotiation, so it cannot veto a codec or a track. Publishing
  rights are fixed in the grant (publish and subscribe, all sources); narrowing them is a grant
  change.
- Leaving and failure are observed by LiveKit. The other peer hears of a dead one from LiveKit
  (20.0 to 21.9 s for a frozen browser over seven runs of tests/call, against 20 s derived from
  10 s ICE disconnected + 5 s ICE failed + 5 s cleanup). If the chat side needs to know, to end
  the call there, it has to subscribe to LiveKit's webhooks; that is the call handler's work.
- Monitor RoomService latency and errors (`Unavailable` versus `Refused`), ticket issue rate, and
  LiveKit's version against the SDK version the clients load.
- Reopen if LiveKit ships a server-side offer/answer API for participants, or if the SFU is
  replaced by one whose signalling we must terminate ourselves.
