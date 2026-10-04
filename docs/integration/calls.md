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
  members only. A group chat, a stream's live chat, or a room with no kind recorded has none
  (`not_callable`).
- The call holds **two participants**. A participant is a device: each member on one device. A
  second device of the same member takes the other member's place, and LiveKit refuses a third.
- The first ticket of a call **rings** the other member on every socket they have open, on
  any node, whether or not that socket joined the room ([Ringing](#ringing)). Either member may
  call; both land in the same call.

## The flow

<!-- apps/chat/src/envelope.hpp (Call, write_ticket), apps/chat/src/chat_service.cpp (ChatService::call, called), infra/sfu/livekit/src/livekit_sfu.cpp (kTicketTtl), apps/chat/src/ring.cpp (Ringer::ticketed) -->

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
   {"type":"ticket","room":"0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0d","url":"wss://<media host>","token":"eyJ...","expires_at":1790000060,"call":"01a0f3c2-55d1-7e2a-9b3c-4d5e6f708192"}
   ```

   | Field | Meaning |
   |---|---|
   | `url` | LiveKit's signalling endpoint; the SDK connects to `<url>/rtc` |
   | `token` | Admits this user and device to this call, once |
   | `expires_at` | Unix seconds, 60 s after issue: connect before then |
   | `call` | The call this ticket belongs to: the one it started ringing, or the one it answered or rejoined. Absent when there is none: nobody else is on the room's member list, or the call you were answering ended while you asked (then do not connect) |

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
5. To leave, `room.disconnect()` and send `call_end` ([Ringing](#ringing)). The other member
   learns of the hang-up from LiveKit (`ParticipantDisconnected`, within about 20 s when a device
   vanishes without leaving), whatever chat says. `call_end` takes effect only while chat still
   holds the answered call, which is 2 minutes past the last ticket asked in it: then the other
   member's other devices hear `call_ended`, and the next call rings at once. After that the call
   is already forgotten and `call_end` is answered `no_call`, which here means it is already
   done: nothing to show, nothing to retry.
6. To come back (the SDK gave up reconnecting, the app restarted, or `room.connect` failed after
   `expires_at`), ask chat for a new ticket (step 2). A connect refused with "room does not
   exist" means the call's room stood empty long enough for LiveKit to drop it: ask again, and
   the new ticket brings it back.

## Ringing

<!-- apps/chat/src/ring.hpp (RingLimits), apps/chat/src/ring.cpp (Ringer, ring_limited, answering), apps/chat/src/call_bell.cpp (CallBell::on_notice), apps/chat/src/envelope.cpp (write_call_event, call_move_of), docs/adr/0091-a-calls-ring-from-the-rooms-owner-through-presence-rooms.md -->

The room's owner keeps one call per direct chat, from the first ticket until it ends, and tells
every open socket of **both** members, on any node, each time it changes. A socket hears these
whether or not it joined the room; it only needs to be open. Each event names the `call`; match
events by it, ignore those of a call you do not know, and treat each `type` for a `call` as
idempotent (you may hear one twice: the ringing is announced again every 15 s, and your own
`call_decline`, `call_cancel` or `call_end` is answered with the same event the others hear).

Server to client, unasked:

| `type` | Fields | Meaning |
|---|---|---|
| `call_ringing` | `room`, `call`, `from`, `expires_at` | `from` is calling. On the caller's own devices (`from` is you), the call you or your other device started is ringing. It rings until `expires_at` (Unix seconds); stop ringing then even if nothing else arrives |
| `call_answered` | `room`, `call`, `from`, `by` | `by` asked for a ticket and is joining. On `by`'s other devices: stop ringing |
| `call_declined` | `room`, `call`, `from`, `by` | `by` turned the call down |
| `call_cancelled` | `room`, `call`, `from`, `by` | The caller gave up before anyone answered |
| `call_missed` | `room`, `call`, `from` | Nobody answered within the ring timeout (45 s) |
| `call_ended` | `room`, `call`, `from`, `by` | `by` ended the answered call |

```json
{"type":"call_ringing","room":"0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0d","call":"01a0f3c2-55d1-7e2a-9b3c-4d5e6f708192","from":"user-42","expires_at":1790000045}
{"type":"call_answered","room":"0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0d","call":"01a0f3c2-55d1-7e2a-9b3c-4d5e6f708192","from":"user-42","by":"user-7"}
```

Client to server, on a socket that joined the room (`not_joined` otherwise):

| `type` | Fields | Who | Meaning |
|---|---|---|---|
| `call_decline` | `room`, `call` | The callee, while it rings | Turn it down: everyone hears `call_declined` |
| `call_cancel` | `room`, `call` | The caller, while it rings | Give up: everyone hears `call_cancelled` |
| `call_end` | `room`, `call` | Either member, once answered, within 2 minutes of the call's last ticket | End it: everyone hears `call_ended`. Later the call is already forgotten, and this is `no_call` |

Each is answered on the socket that sent it with the event everyone hears, or an
[error](#errors): `no_call` when the room has no such call in a state you may move it from
(already ended or forgotten, answered for a decline or a cancel, still ringing for an end, or
someone else's to cancel). For `call_end` it means the call is already over as far as chat
knows: chat forgets an answered call 2 minutes after its last ticket, while LiveKit may still
carry it. Treat it as done. Each is checked against the member list on the room's owner, like a ticket, and
counted with joins.

**Client states.** The caller: *calling* from the ticket (connect to LiveKit at once and wait
there), *in call* on `call_answered`, back to idle on `call_declined`, `call_missed` or
`call_ended`, or after its own `call_cancel`. The callee: *ringing* on `call_ringing` (show the
caller, ring until `expires_at`); to answer, `join` the room if this socket has not, ask for a
ticket (`call`) and connect: that is `call_answered` for everyone; to decline, `join` and
`call_decline`. Every other device of the callee goes idle on `call_answered`, `call_declined`,
`call_cancelled` or `call_missed`.

What the ring does not cover:

- **Ringing too often.** A direct chat starts at most 5 rings a minute, whoever calls, and a
  caller whose call was declined may not ring that member again for 30 s (the declining member
  may call back at once, which lifts the wait). A ticket past either is refused `ring_limited`
  before anything rings.
- **Answering at the last moment.** A callee's ticket asked before `expires_at` holds the ring
  up to 10 s past it while LiveKit issues it, so the answer is not lost to `call_missed`: the
  caller may hear `call_answered` a few seconds after `expires_at`, and the callee's own
  devices should keep the call they answered even past `expires_at`.
- **A member with no socket open hears nothing**: there are no push notifications yet. A
  device that connects while the call still rings hears the next announcement, within 15 s.
- **Rejoining.** Asking for a ticket again within 2 minutes of the last one in an answered call
  joins it without ringing anyone. Later, it rings the other member: a client already connected
  to the room's LiveKit call that hears `call_ringing` for that room from the other member
  answers it by asking for a ticket (and keeps its connection), so both stay in the call.
- **A change of the room's owner** (a deploy, a node failure) forgets a ringing call without a
  word: devices stop at `expires_at`, and a decline or cancel then is `no_call`. A callee who
  answers by asking for a ticket rings the caller as a new call, which the caller's client,
  connected to LiveKit and waiting, answers as above.

## Errors

<!-- apps/chat/src/chat_service.cpp (ChatService::called, ChatService::moved), apps/chat/src/call.hpp (CallOutcome) -->

Each is an `error` with the `room` of the call.

| `reason` | Meaning | Client action |
|---|---|---|
| `malformed`, `bad_room`, `bad_device`, `bad_call` | The command is not a call: a field missing or extra, a room, device or call that is not a canonical lowercase UUID | Fix the client |
| `not_joined` | This connection has not joined the room | `join` first |
| `not_member` | You are not on the direct chat's member list (also when removed since your `join`) | Do not retry |
| `not_callable` | The room is not a direct chat | Do not retry: group calls are not available |
| `unavailable` | LiveKit, the member list or the room's owner could not be reached; `retry_after_ms` says when to ask again | Wait that long, then ask again |
| `busy` | The call allowance (counted with joins: a burst of 64, then one a second per user) or the room's owner is at its limit (asks in flight, or 4096 calls ringing or answered) | Back off and retry |
| `no_call` | A `call_decline`, `call_cancel` or `call_end` for a call the room does not have in that state ([Ringing](#ringing)): for `call_end`, usually an answered call chat already forgot (2 minutes after its last ticket) | Treat the call as done; do not retry. The other member learns of a hang-up from LiveKit |
| `ring_limited` | The ticket would ring the other member, and this direct chat may not ring yet: it rang 5 times in the last minute, or the other member declined your call less than 30 s ago. `retry_after_ms` says when it may | Do not ring again before then; tell the user. Nothing was rung and no ticket issued |
| `call_failed` | LiveKit refused the request as made: a fault on the service's side | Retry much later; report it |
| `calls_disabled` | Calls are not configured on this deployment | Do not retry |

A removed member who is already connected to the call stays in it until they disconnect; they
cannot get another ticket, nor decline, cancel or end a call. Putting someone out of a call is planned with group calls
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
