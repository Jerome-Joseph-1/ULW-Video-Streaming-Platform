# 0091. A call's ring lives on the room's owner and reaches each member through their presence room

Status: Accepted
Date: 2026-10-03
Amends: ADR-0035 (the node channel carries unsequenced notices; version 5); ADR-0087 (the first
ticket of a call rings, and the call has decline, cancel and end)

## Context

ADR-0087 ships 1:1 call tickets: a member of a direct chat asks the room's owner for a ticket
over the room WebSocket and connects to LiveKit with it. Nothing tells the other member a call
is starting: calls.md told clients to say so in a chat message of their own, and there was no
way to turn a call down, give up ringing, or say it ended. A real user cannot be called.

What a ring needs, and what the system gives today:

- **Who hears it.** Every socket of the other member, on whichever node, joined to the direct
  chat or not: an app does not join every conversation (64 rooms a socket, joins at one a second
  past a burst). The room plane delivers a room's messages only to sockets that joined that room.
  Presence (ADR-0056) does reach a user wherever they are: every node with a socket of the user
  is in the user's presence room, from connect until the grace runs out.
- **Where the state lives.** Ringing, answered, declined, cancelled and missed are one state
  per call, with a timeout. The owner of the direct chat's room already answers every ticket
  (ADR-0087) and checks the member list for it; two nodes keeping the state would disagree.
- **What travels.** Ring events must not be stored as chat messages: they are not part of the
  conversation's history, and a sequenced write is a fenced store write per event.
- **Bounds.** Every structure is bounded (ADR-0036), and every deadline is met without sleeping
  on the reactor.

## Options

| Question | Option | Verdict |
|---|---|---|
| How events reach the other member | As messages in the direct chat's room | Rejected: only sockets that joined the room hear them, and they would be stored and sequenced as conversation |
| | A per-user inbox room every connected node joins | Rejected: a second room per connected user, with its registry claim and heartbeat place, to carry what presence rooms already reach |
| | Presence room events (Presence's own 9-byte events, sequenced) | Rejected: a fenced store write per event, and Presence would have to parse and forward call events, which are not presence |
| | An unsequenced **notice**: any node hands bytes to a room's owner, which passes them to every node subscribed to the room, and each hands them to a listener; sent to the member's presence room | Accepted: the presence room already reaches every node the member is on; nothing is stored or sequenced; rt stays ignorant of calls, as it is of tickets (ADR-0087) |
| How a node that is not in the presence room finds its owner | Resolve it, as a join does | Rejected: resolving claims a room nobody owns, so ringing someone offline would make the ringing node own their presence room |
| | Read the owner (`read_owners`, read only) and send to it; no owner means nobody is connected | Accepted: one indexed read per notice the node has no route for; a user nobody has a socket of costs a read and nothing else |
| Where the ring's state lives | The direct chat's owner, beside the ticket handler | Accepted: tickets already go there, with the member list read on the owner; one place decides the race of an answer with a cancel |
| When the ring starts | When the first ticket is asked | Rejected: a caller LiveKit turns away would ring the other for a call that cannot happen |
| | When the first ticket is issued | Accepted |
| What answers | A new `call_answer` command | Rejected: the callee asks for a ticket to join anyway; asking is answering, as the brief says |
| | The callee's ticket | Accepted |
| Timeouts | A timer per call | Rejected: the handler already runs after every turn of the loop (its sweep); one ordered set of deadlines is cheaper and keeps the clock injected |
| | Deadlines in an ordered set, read by the handler's sweep | Accepted |
| Ringing as often as a client likes | Allowed: each ring needs a ticket, already counted with joins | Rejected: a loop of ticket and cancel rings the other member every 2 s for as long as the join allowance lasts, which at one a second is forever |
| | Per room on the owner: at most 5 ring starts a minute, and 30 s before a declined caller may ring the same member again, refused `ring_limited` with `retry_after_ms` | Accepted: the owner decides every ring already; the room is the pair being rung, whoever calls |
| An answer arriving at the ring's timeout | Rung out at the timeout, whatever is in flight | Rejected: a callee who answers a second before `expires_at` waits on the SFU for up to 10 s and loses the call to `call_missed` |
| | Mark the call answered when the callee's ask arrives, roll back if the SFU fails | Rejected: the caller would hear `call_answered` and then, on a failure, a second ending |
| | A callee's ask arriving before the timeout holds the ring up to 10 s past it, until its ticket answers it | Accepted: nothing is announced until it is true; an ask that never becomes a ticket rings out at the end of the grace |
| An owner change mid-ring | Move the state with the room | Rejected: ADR-0087 deferred the stored generation for want of anything that moves it; moving ring state is the same order of work for a 45 s window |
| | The deposed owner forgets its calls without a word; clients stop ringing at `expires_at` | Accepted |

## Decision

- **Notices on the room plane.** `RoomRouter::notify(room, bytes)` hands at most
  `kMaxOwnerMessage` (4 KiB) to the room's owner; the owner passes it to its own listener when it
  has members in the room, and as a `Notice` frame to every node subscribed to it, whose
  listener (`RoomRouter::hear`) gets it once. Unsequenced, unstored, at most once per node, best
  effort. A node with no route reads the owner without claiming; up to 256 rooms' lookups of 8
  notices each wait at once, and a notice past that, or without an owner, is dropped and counted
  (`notices_total{stage="dropped"}`). An owner that no longer holds the room drops a `Notify`
  it is handed; it never resolves the room. Two frames, `Notify` (any node to the owner) and
  `Notice` (owner to subscriber), move the node channel to **version 5**.
- **Presence rooms carry the ring.** Each event is a notice to the presence room of each member
  of the call, the caller included, naming that member. On every node the call bell
  (`call_bell.cpp`) pushes it, as JSON, to that member's sockets there, and drops a notice whose
  room is not its member's presence room.
- **The ring on the owner** (`ring.cpp`). A room has at most one call. The first ticket issued
  in a room with no call reads the member list (on the owner, at the moment of asking, like the
  ticket's own check) and rings every other member: `call_ringing` with the call's id and
  `expires_at`. A callee's ticket answers it (`call_answered` to both; the callee's other
  devices stop ringing); `call_decline` (a callee), `call_cancel` (the caller) and, once
  answered, `call_end` (either) end it; with nobody answering for the ring timeout (45 s,
  `ULW_CALL_RING_TIMEOUT_MS`, 1 to 300 s) both are told `call_missed`. A ringing call is
  announced again every 15 s, for sockets that connected meanwhile and notices that were lost.
  An answered call is kept 120 s past its last ticket, so a member asking again (a restart, a
  second device) joins it without ringing anyone. Calls are matched by id: a signal for another
  call, or from the wrong member, is `no_call`.
- **Authorization.** Decline, cancel and end are asks to the owner like tickets: the router
  takes them only from a socket joined to the room, and the owner reads the room's kind and the
  asker's membership again (`IMessageStore::access`) for each (`not_member`, `not_callable`).
- **Bounds.** 4096 calls ringing or answered per owner (`busy` for a ticket that would start
  another, checked before the SFU is asked and again when the ticket is issued); at most seven
  members rung per call, from a list read up to eight; the router's notice lookups above. The
  answer layouts change (layout 2): an ask says what it asks, a ticket names its call.
- **Ring rate.** Per room, on the owner: at most 5 rings start within a minute
  (`RingLimits::rings_per_window`, `ring_window`), and a caller whose call a callee declined may
  not ring the room again for 30 s (`decline_cooldown`); the callee may call back at once. A ticket
  past either is refused before the member list or the SFU is asked, and checked again when the
  ticket is issued: `ring_limited` with `retry_after_ms`, counted as
  `call_refusals_total{reason="ring_limited"}`. Each room's recent ring starts are remembered
  in a fixed buffer of at most 16 (`kMaxRingsPerWindow`; `rings_per_window` is held to 1 to 16),
  about 340 bytes a room with the map's own, for 16384 rooms at most (5.5 MiB), forgotten once no
  limit needs them; past it a ring is `busy`, and the forgetting runs at most once a second. A
  declined caller's cooldown ends early when the member who declined rings the room themselves.
- **Answer grace.** A callee's ticket ask that reaches the owner before the ring's timeout holds
  the call up to 10 s past it (`answer_grace`): its ticket answers the call when the SFU issues
  it. An ask that fails rings out at the end of the grace (`call_rings_total{outcome="graced"}`
  counts the held rings) The grace is decided when the owner's handler checks the ask, after its read of the
  room's access, not when the ask leaves the client: a very slow read can push an ask that was
  sent in time past the timeout, and the call rings out.
- **Owner changes.** A node that no longer owns a room forgets its calls at their next
  deadline without a word (`call_rings_total{outcome="orphaned"}`). The new owner knows none:
  a decline or cancel is `no_call`, and the callee's ticket rings the caller as a new call, which
  a caller already in the call answers by asking for a ticket (calls.md).
- **Offline members** hear nothing: no node holds their presence room, the owner lookup finds
  none, and the notice is dropped. Push notifications are out of scope; a client coming online
  during the ring hears the next announcement.

## Consequences

- A release that carries this moves the node channel from version 4 to 5: roll it out with the
  chat Deployment's strategy set to `Recreate` (deploy/kubernetes/RUNBOOK.md, section 3). If it
  ships in the same
  release as ADR-0087's version 4, that one Recreate covers both.
- Every ring event costs one notice per member, and a read of the owner of each member's
  presence room on the ringing node (it routes none of those rooms). A call is a handful of
  events. The reads are not cached: the re-announcement every 15 s alone is two reads per ringing
  call, so at the cap of 4096 ringing calls an owner makes about 550 owner reads a second
  (4096 x 2 / 15), each one indexed row. Cache the owners (with the router's revalidation) if
  that becomes real load.
- `call_end` takes effect while the owner holds the answered call, 120 s past its last ticket;
  later it is `no_call`, which a client takes as done. The other member learns of a hang-up from
  LiveKit (`ParticipantDisconnected`) either way.
- A member who comes back to an answered call after its 120 s hold rings the other, who is in
  the call: calls.md tells clients already connected to a room's call to answer such a ring by
  asking for a ticket. LiveKit's room events (participant left, room finished) would let the
  owner know when a call really ends; reopen this when they reach chat.
- An owner lost mid-ring leaves devices ringing until `expires_at` with no `call_missed`; the
  caller's client stops on the same deadline.
- Reopen for push notifications (a member with no socket hears nothing), for group calls
  (ADR-0058: a ring of many, and who may end it), and if notices need more than best effort.
