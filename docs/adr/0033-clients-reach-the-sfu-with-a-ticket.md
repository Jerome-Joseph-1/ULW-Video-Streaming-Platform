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

- The port is `ISfu::open_room(RoomId, MediaGeneration, max_participants) -> IMediaRoom`,
  `IMediaRoom::join(UserId, DeviceId) -> MediaTicket` (asynchronous) and `IMediaRoom::close()`
  (`core/include/core/ports/media.hpp`). A `MediaTicket` is an endpoint, an opaque credential
  and its expiry; core never sees a LiveKit word. `apply_offer`, `add_ice_candidate` and the SDP
  they carry are gone from the port.
- Signalling over the room WebSocket is: the client asks to join a call; the call handler checks
  membership (the JWKS-authenticated user, ADR-0018), opens the room (idempotent) on the owning
  node, and answers with the ticket. Offers, answers and candidates run between browser and
  LiveKit. `codec/sdp` stays useful as test tooling and to validate SDP captured from LiveKit
  sessions, not as a relay.
- `infra/sfu/livekit` implements the port. Each generation of a room is the LiveKit room
  `<room id>:<generation>`, created with `max_participants`; a participant is the identity
  `<user>/<device>`, so two devices of one user are two participants. Tokens are HS256 JWTs with
  LiveKit's `video` grant, minted in process from `LIVEKIT_API_KEY`/`LIVEKIT_API_SECRET`: a join
  token grants `roomJoin`, publish and subscribe in one generation and nothing else. It lives
  60 s, the SDK's connect budget (15 s signal plus 15 s peer connection) with its one retry: the
  ticket is presented once, because LiveKit sends the client a fresh token the moment it joins
  and renews it every 5 minutes, each good for 10 minutes (`refreshToken`,
  `pkg/service/roommanager.go:765` and `1152-1166` in v1.13.7), and reconnects use that. Each
  RoomService call carries its own 10 s token with only the permission it needs. RoomService
  (Twirp JSON over HTTP) runs on the reactor through `infra/curl`.
- LiveKit deletes a room that stands empty: 60 s after creation if nobody joined
  (`empty_timeout`), 60 s after the last participant left (`departure_timeout`). The first covers
  a ticket issued as the room opens; the second the SDK's reconnect after everyone's link drops
  at once, whose retry delays add up to 44.1 s. A room handle outlives that easily (a callee's
  phone ringing), so `join` re-creates the generation (CreateRoom is idempotent and carries the
  same settings) before it mints the ticket, one round trip per join, and a ticket never names a
  room that is gone. Only a room that was about to be swept can still vanish between the ticket
  and the connect; the client is refused ("room does not exist") and asks again, and the next
  join re-creates it. A handle that `close()` was called on refuses to join (`Closed`) instead,
  so a closed generation is never re-created by its own handle.
- **Putting a participant out is closing a generation.** No credential LiveKit has issued can be
  withdrawn: a client removed with `RemoveParticipant` reconnects with the token LiveKit
  refreshed for it and is issued another (the review reproduced this, and tests/call keeps the
  probe), and LiveKit v1.13.7 has no call that revokes a token or stops the refresh; the
  refreshed token copies the participant's grants (`roommanager.go:1146-1177`). What LiveKit
  does refuse is a room that does not exist, since `auto_create` is off. So the call handler
  expels, or applies a revoked membership, by moving the call on: it opens generation N+1,
  sends the members who stay a ticket for it, which their clients connect with, and closes N, which disconnects everyone still in N and leaves every
  credential naming N useless. Generations only move forward, so N never exists again.
- The generation belongs to the caller, stored with the room's state (ADR-0015). It moves only
  through the owner's fenced write, and the SFU is told only after that write commits: opening
  N+1, closing N and ending the call each follow a fenced update of the generation or of the
  call's state, and a deposed owner's update matches no row, so it never touches the SFU. Tickets
  are issued for the generation the owner last wrote. Closing a generation that LiveKit has
  already dropped succeeds.
- TURN (STUNner, ADR-0013) is not an SFU concern: the call handler sends the per-session TURN
  credentials next to the ticket, and the client hands them to the SDK as its ICE servers with a
  relay-only policy. That is why `IceCredentials` left `join`.
- The browser loads `livekit-client`, pinned to an exact version by lockfile (2.22.3 in the call
  suite).

## Consequences

- Clients carry a LiveKit SDK; the ticket is the only LiveKit-shaped thing we send them.
  Replacing the SFU changes the client SDK, the adapter and the ticket's contents, but not the
  port or the room WebSocket.
- Expelling one participant reconnects every other one. For a 1:1 call that is one peer; for
  the group calls of M27 it is the whole room, once per expulsion, which is rare by nature.
- Membership changes that only add people need no new generation; only taking someone out does.
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
