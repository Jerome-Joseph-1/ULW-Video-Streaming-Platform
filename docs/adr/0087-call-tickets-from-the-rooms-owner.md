# 0087. A call's ticket comes from the room's owner, asked over the node channel, in one generation

Status: Accepted, amended by 0091 (the first ticket rings the other member; decline, cancel and end) and 0095 (the generation is stored, and moves to put someone out)
Date: 2026-10-03
Amends: ADR-0035 (the node channel carries asks a room's owner answers; version 4); ADR-0050
(how the call handler reaches the owning node, and the generation before expulsion exists)

## Context

ADR-0050 settles what a call's signalling is: a client asks to join a call over the room
WebSocket; the call handler checks membership, opens the room's media room on the owning node,
and answers with a ticket. Nothing deployed did it: the LiveKit adapter was driven only by the
call suite's harness (`tests/call/harness_main.cpp`), so 1:1 calls (M23 to M26) could not be
used. Four things ADR-0050 leaves open had to be settled to ship the handler:

- How a request reaches the owning node. The node channel (ADR-0035) carries subscriptions,
  sends and deliveries only, each with a meaning of its own to the room plane.
- Which rooms have a call. The chat kinds are direct chat, group chat and a stream's live chat
  (`core::ports::RoomKind`); ADR-0058 parks group calls.
- Where membership is read. A client's `join` was checked when it was made; a removal reaches
  the client's node by notification (ADR-0073), which may still be on its way.
- Which generation a call runs in. ADR-0050 stores it with the room's state and moves it by the
  owner's fenced write, to put someone out. Nothing puts anyone out yet, and the room's state
  has no column for it.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Any node opens the media room and issues the ticket itself, no hop | Simplest; `open_room` and `join` are idempotent, so two nodes opening one generation agree | Rejected: ADR-0050 puts the handler on the owner, where the generation and later the expulsion live (fenced writes, ADR-0015); a handler on every node would have to move there the day expulsion lands, and clients would see the difference in failure modes then |
| A call-specific frame on the node channel (CallJoin, CallTicket) | Typed end to end | Rejected: the room plane would learn what a ticket is; the next owner-side request (expel, end the call, a stream's publisher ticket) would add frames again |
| A generic ask: the router carries opaque bytes from a member's node to the room's owner and back (`RoomRouter::ask_owner`, `IOwnerService`), and the call handler is the owner's service | rt stays ignorant of calls; a forwarded ask gets the same routing, NotOwner handling and member guard as a send; later owner-side requests reuse it | Accepted |
| Membership from the client's join alone | No store read per call | Rejected: the join may predate a removal still in flight to the client's node, and the owner is where a removal must be decided anyway (ADR-0050) |
| The owner reads the room's recorded kind and member list for every ask (`IMessageStore::access`, read only) | One indexed read, a millisecond; authoritative; tells a direct chat from the rest | Accepted |
| Store the generation now (a column on `room_state`, moved by fenced write) | The final shape | Deferred: with no expulsion nothing moves it, so it would be a column only ever 1, and its migration and fenced write belong with the code that moves it |

## Decision

- **The ask.** A client sends `{"type":"call","room":...,"device":...}` on the room WebSocket
  for a room its connection has joined (`not_joined` otherwise, as for `send`). The node asks
  the room's owner through `RoomRouter::ask_owner`: on the owner itself the call handler answers
  in process; elsewhere the ask goes as an `Ask` frame and comes back as an `Answer` frame, which
  moves the node channel to version 4. An ask waits at most 15 s (`kOwnerAskTimeout`: a store
  read and two SFU calls of up to 5 s each), on the owner as much as from another node, and is
  then answered `unavailable`; like a send, it is dropped if the member leaves the room first. Each ask is charged as a join (burst 64, then 1/s per user).
- **The owner's check.** The handler reads the room's recorded kind and the asker's membership
  (`IMessageStore::access`) at the moment of asking. Only a direct chat has a call
  (`not_callable` for a group chat, a stream's live chat or a room with no kind recorded); only
  a listed member gets a ticket (`not_member`).
- **One generation per call, for now.** Every call runs in generation 1. The owner opens the
  media room once (`open_room(room, 1, Call, 2)`), keeps the handle for 5 minutes after its last
  ask, and reuses it; asks that arrive while it opens wait for that open. A deposed owner's
  handle names the same generation, so it is harmless, and a new owner simply opens it again
  (idempotent). Each ticket is `IMediaRoom::join(user, device, Member)`, which re-creates the
  room first (ADR-0050).
- **1:1 is two participants.** The media room's cap is 2, and a participant is a device
  (ADR-0050): the two members, one device each. A second device of the same user takes the
  second place.
- **Failures.** The SFU or the member list unreachable answers `unavailable` with
  `retry_after_ms` (2000); the SFU refusing the request as made answers `call_failed`; a node
  without `LIVEKIT_API_KEY` answers `calls_disabled`, so a deployment without LiveKit runs as
  before.
- **Leaving and expelling are later.** Leaving needs nothing of ours (the client disconnects
  from LiveKit, which tells the other peer). Putting someone out, and ending a call when a member
  is removed from the direct chat, is ADR-0050's generation move: it needs the stored generation
  and the owner's fenced write, and arrives with them.

## Consequences

- Until expulsion lands, a member removed from a direct chat while connected to its call stays
  in the call until they disconnect, since LiveKit keeps refreshing their credential (ADR-0050);
  they cannot get a new ticket. Acceptable for 1:1 (the two people were talking already); not
  for group calls, which ADR-0058 keeps parked.
- A release that carries this changes the node channel's version (3 to 4): it rolls out with
  the chat Deployment's strategy set to `Recreate` (RUNBOOK, section 3).
- The node channel can carry any request a room's owner answers, at most 4 KiB each way. The
  next owner-side features (expel, end a call, a stream's publisher ticket) need a handler, not
  a frame.
- Every call costs the owner one store read and, on LiveKit, one `CreateRoom` per ticket (two
  for the first ticket of a handle). Watch `call_errors_total` and `call_tickets_total`.
- Reopen when expulsion is built (store the generation then), or if asks need to outlive an
  owner change (they are answered NotOwner and retried by the client today).
