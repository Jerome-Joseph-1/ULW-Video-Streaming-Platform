# Calls

> **Draft until M26 is tagged.** The flow below is what chat serves wherever LiveKit is
> configured (deploy/kubernetes/RUNBOOK.md, step 7). A deployment without LiveKit answers every
> call with `calls_disabled`.

> **Group calls are not available.** They are planned (ADR-0058) and nothing serves them: do not
> build against them. Only 1:1 calls are described below. This page will say when that changes.

Calls go through LiveKit as the SFU (ADR-0020), with media entering the cluster through STUNner
(ADR-0013), and are always relayed. The service does not expose LiveKit's server API: a client
asks chat for a call on its room WebSocket ([chat.md](chat.md)), and the chat node that owns the
room checks the asker is a member of the direct chat, opens the room's call on LiveKit and
answers with a short-lived ticket (ADR-0050, ADR-0087). The client then connects with the
LiveKit JS SDK using that ticket, and LiveKit owns ICE, DTLS-SRTP, RTP and congestion control
from there. TURN credentials come from LiveKit itself, in its answer to the SDK's join
(ADR-0037): neither the service nor client code handles them.

## Which rooms have a call

<!-- apps/chat/src/call.cpp (CallHandler::checked), apps/chat/src/call.hpp (kCallParticipants), docs/adr/0087-call-tickets-from-the-rooms-owner.md -->

- A **direct chat** (`"kind":"direct"`, [chat.md](chat.md#member-lists)) has one call, for its
  members only. Open one with `open_direct` ([chat.md](chat.md#changing-member-lists)) and
  join the room it answers. A group chat, a stream's live chat, or a room with no kind recorded has none
  (`not_callable`).
- The call holds **two participants**. A participant is a device: each member on one device. A
  second device of the same member takes the other member's place, and LiveKit refuses a third.
- There is **no ringing signal**. The service does not tell the other member a call started:
  tell them through the room (a chat message your app defines), and they ask for their own
  ticket. Either member may ask first; both land in the same call.

## The flow

<!-- apps/chat/src/envelope.hpp (Call, write_ticket), apps/chat/src/chat_service.cpp (ChatService::call, called), infra/sfu/livekit/src/livekit_sfu.cpp (kTicketTtl) -->

1. Open the chat WebSocket and `join` the direct chat's room, as for messages
   ([chat.md](chat.md)). A call is asked on a room this connection has joined.
2. Ask for a ticket:

   ```json
   {"type":"call","room":"0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0d","device":"0192f0c5-1b2c-7d3e-8f40-123456789abc"}
   ```

   `device` is a canonical lowercase UUID the client makes once per install (or browser
   profile) and keeps: it tells two devices of one user apart.
3. The answer is a ticket:

   ```json
   {"type":"ticket","room":"0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0d","url":"wss://<media host>","token":"eyJ...","expires_at":1790000060}
   ```

   | Field | Meaning |
   |---|---|
   | `url` | LiveKit's signalling endpoint; the SDK connects to `<url>/rtc` |
   | `token` | Admits this user and device to this call, once |
   | `expires_at` | Unix seconds, 60 s after issue: connect before then |

4. Connect with the LiveKit JS SDK (`livekit-client`; the call suite pins 2.22.3), before
   `expires_at`:

   ```js
   import { Room, RoomEvent } from 'livekit-client';
   const room = new Room();
   room.on(RoomEvent.TrackSubscribed, (track) => attach(track));        // the other member
   room.on(RoomEvent.ParticipantDisconnected, () => showCallEnded());
   room.on(RoomEvent.Disconnected, () => offerToRejoin());
   await room.connect(ticket.url, ticket.token);
   await room.localParticipant.enableCameraAndMicrophone();
   ```

   The ticket is used once. After the connect LiveKit keeps the connection's credential fresh
   itself, and the SDK reconnects with it after a network change; do not store the ticket.
5. To leave, `room.disconnect()`. Nothing is sent to chat: LiveKit tells the other member
   (`ParticipantDisconnected`), within about 20 s when a device vanishes without leaving.
6. To come back (the SDK gave up reconnecting, the app restarted, or `room.connect` failed after
   `expires_at`), ask chat for a new ticket (step 2). A connect refused with "room does not
   exist" means the call's room stood empty long enough for LiveKit to drop it: ask again, and
   the new ticket brings it back.

## Errors

<!-- apps/chat/src/chat_service.cpp (ChatService::called), apps/chat/src/call.hpp (CallOutcome) -->

Each is an `error` with the `room` of the call.

| `reason` | Meaning | Client action |
|---|---|---|
| `malformed`, `bad_room`, `bad_device` | The command is not a call: a field missing or extra, a room or device that is not a canonical lowercase UUID | Fix the client |
| `not_joined` | This connection has not joined the room | `join` first |
| `not_member` | You are not on the direct chat's member list (also when removed since your `join`) | Do not retry |
| `not_callable` | The room is not a direct chat | Do not retry: group calls are not available |
| `unavailable` | LiveKit, the member list or the room's owner could not be reached; `retry_after_ms` says when to ask again | Wait that long, then ask again |
| `busy` | The call allowance (counted with joins: a burst of 64, then one a second per user) or the room's owner is at its limit | Back off and retry |
| `call_failed` | LiveKit refused the request as made: a fault on the service's side | Retry much later; report it |
| `calls_disabled` | Calls are not configured on this deployment | Do not retry |

A removed member who is already connected to the call stays in it until they disconnect; they
cannot get another ticket. Putting someone out of a call is planned with group calls
(ADR-0050, ADR-0087).

## Capacity

Measured, not modelled (tests/load/call_capacity, e2e run 36589275099): one 1:1 call, each side
publishing and receiving audio and 720p video, costs the SFU 0.0158 cores and 1.504 Mbit/s in
each direction. CPU binds first. At the 2-core limit of the LiveKit deployment
(deploy/kubernetes/base, in every overlay) that is about 126 concurrent 1:1 calls per SFU pod;
the 540 Mbit/s bandwidth ceiling of ADR-0012 would allow 359 at the measured rate (385 at the
1.4 Mbit/s that ADR assumed), so it is not what limits a pod. Rooms of more than two cost more
per call (downstream legs grow as n x (n - 1), ADR-0012); the figures above do not cover them.

ADR-0012's bandwidth arithmetic stands as the ceiling, and it defers CPU to measurement; the
0.0158 cores per call is that measurement. The run used a shared CI runner, so treat the CPU
figure as an estimate to firm up on a bigger one rather than a guarantee.
