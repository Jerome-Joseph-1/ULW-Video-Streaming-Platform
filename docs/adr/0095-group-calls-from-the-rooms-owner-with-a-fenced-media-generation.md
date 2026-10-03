# 0095. Group calls from the room's owner, with a media generation moved by its fenced write

Status: Accepted
Date: 2026-10-04
Implements: ADR-0058 (group calls, interfaces now and implementation deferred)
Amends: ADR-0087 (a call's generation is stored, and moves); ADR-0091 (a group call's ring);
ADR-0050 (who puts a participant out, and how a deposed owner is kept from the SFU)

## Context

ADR-0058 parked group calls (M27 to M29) with the media port shaped for them: `open_room` takes a
participant cap, putting someone out is closing a generation (ADR-0050), and
`IMediaRoom::participants` was added and left `NotImplemented`. ADR-0087 shipped 1:1 calls in
one generation, never stored, for want of anything that moved it; ADR-0091 added the ring for a
direct chat. The plan's criteria, as ADR-0058 kept them:

- M27: four peers each see three inbound streams; a forced owner reassignment mid-call is a
  bounded interruption followed by full recovery; the peers are spread over two signalling nodes.
- M28: a throttled receiver downshifts layers within a measured time; induced loss shows
  NACK then retransmission in a capture; PLI then keyframe within a measured time; the bandwidth
  estimate converges within a measured tolerance of the cap.
- M29: measured egress matches the derivation within 10%, with the simulcast layer stated.

What a group adds to a 1:1 call, on the owner:

- **More than two, and who is in it.** A direct chat's two members either ring or talk. A group
  has members who answer, decline, never hear, leave and come back while the call goes on, and
  a call that should end when nobody is left in it, which only the SFU knows for sure.
- **Putting someone out.** ADR-0050's mechanism is a generation move, made safe by the owner's
  fenced write. The generation now has to be stored, read by a new owner, and moved only by the
  owner, before the SFU is told.
- **Capacity.** n participants cost the SFU n x (n - 1) forwarded streams (ADR-0012).

## Options

| Question | Option | Verdict |
|---|---|---|
| Where the generation lives | In memory on the owner | Rejected: a new owner would start again at 1 and re-create a generation closed to keep someone out |
| | A column of `room_state`, read and moved on under the owner's generation (`media_generation`, migration 0015), through `IRoomStore::media_generation` and `RoomRouter::media_generation` | Accepted: the same fence as every owner write (ADR-0015); a deposed owner neither reads nor moves it, and a fenced step gives the room up like a fenced append |
| When a new owner learns it | From the claim's answer | Rejected: the router would carry a call's number for every room it claims |
| | A fenced read on the first ask after the node took the room, the handle keyed by the owner generation it was read under | Accepted: one indexed read per room and ownership |
| Who may put someone out | Any member | Rejected: anyone could empty a call |
| | The group chat's admins | Deferred: the member lists have no roles yet (ADR-0096 brings them) |
| | The call's caller, and the member list: a member removed from the chat is put out by the owner | Accepted |
| How someone is put out | `RemoveParticipant` | Rejected by ADR-0050: LiveKit keeps refreshing the removed client's credential |
| | Generation move: fenced advance, then close the old generation once the joins asked of it are answered; the others are told `call_moved` and ask for a ticket again | Accepted: interruption measured at 0.9 s for three peers in Chrome |
| | The owner pushes each one who stays a ticket for the new generation | Rejected: the owner knows users, not their devices; asking again costs one round trip and reuses the ticket path |
| Whether a call is still on | Kept 120 s past its last ticket, as a 1:1 call | Rejected for groups: a long call with nobody joining would be forgotten while people talk, and `call_end` would answer `no_call` |
| | LiveKit webhooks | Rejected for now: a public endpoint and its secret for one bit of state |
| | The owner asks the SFU (`participants`, LiveKit's `ListParticipants`) once the hold passes, then every 30 s; nobody connected ends the call | Accepted: one RoomService call per call per 30 s |
| The cap | LiveKit's room `max_participants` alone | Rejected: a device past it learns only that its connect failed |
| | Counted on the owner before each group ticket (`participants`), refused `call_full`; LiveKit's cap stays as the backstop | Accepted |
| How many to ring | Every member | Rejected: a notice per member per event, re-announced every 15 s, for groups of any size |
| | The first 32 others of the member list; anyone else may still join | Accepted |

## Decision

- **Which rooms.** A direct chat's call holds 2 devices (ADR-0087); a group chat's call holds
  `ULW_CALL_GROUP_PARTICIPANTS` devices, 8 by default, 3 to 16 (M29 below). A stream's live chat
  has none (`not_callable`).
- **The generation.** `room_state.media_generation`, from 1, moved by one in the owner's fenced
  write and never otherwise. The owner reads it under its own ownership before its first ticket,
  and again whenever its ownership generation changed; tickets name the generation it last read
  or wrote. The SFU hears of a move only after the write: the old generation is closed once the
  joins in flight on it are answered (their tickets are not handed out), and retried every second
  until LiveKit says it is gone, for up to 5 minutes; a generation another owner opened is
  opened (idempotently) to be closed. The store not answering a move leaves the SFU untouched;
  a removal's move is retried each second, a caller's is answered `unavailable`.
- **The ring of a group** (ADR-0091's ring, extended). The first ticket rings up to 32 other
  members. Each one rung answers by asking for a ticket (`call_answered`, to everyone) or
  declines (`call_declined`, to everyone; the call goes on); at the ring timeout those still rung
  are told `call_missed` alone, and the call goes on for the others. Nobody answering, or everyone
  declining, is `call_missed` for everyone. A member may join a call that is on at any time.
  `call_leave` takes a member out (`call_left`); the last one out ends it (`call_ended`). The
  caller may `call_end` it for everyone (a generation move with no successor) and
  `call_expel` a member (a generation move; `call_moved` with `expelled` to everyone, the one put
  out included); a member removed from the chat is put out the same way by the owner, with no
  `by`. Whoever was put out is refused `expelled` while the call lasts. A target with no ticket in
  the call this owner knows is moved for all the same when the SFU still holds a device of theirs,
  or cannot say.
- **Occupancy.** An answered group call nobody asked a ticket of for 120 s asks the SFU who is
  connected, then every 30 s; nobody ends it (`call_ended`, no `by`).
- **Direct chats.** Unchanged, but for one thing: a member removed from a direct chat during its
  call ends it (generation moved with no successor, `call_ended` with no `by`), which ADR-0087
  left to this.
- **Bounds.** Calls: 4096 per owner (ADR-0091). Per group call: 32 rung, 64 members in it
  (`busy` past), 32 put out remembered (the oldest forgotten). Moves waiting per room: 16
  (`busy`). Old generations awaiting their close: 1024 per node (`busy` for an expulsion past
  it). Each is a vector or a small record on the owner, freed when the call ends.
- **Wire.** No node-channel frame changes, so no `Recreate`: the asks, answers and notices are
  opaque to rt and keep their layouts (2 and 1), with new values appended (signals 4 Leave and
  5 Expel with its target; outcomes 10 Expelled and 11 Full; events 6 Left and 7 Moved with its
  subject). A node of the previous release decodes none of the new values: during a rolling
  deploy a group call asked of an old owner is `not_callable` or `unavailable`, and an old node
  pushes no `call_left` or `call_moved`. 1:1 calls are unaffected.
- **M28 is client code.** The SFU adapts per subscriber (ADR-0050); the client publishes with
  simulcast and dynacast. `tests/call/adaptation.sh` measures it in Chrome, one peer in a
  network namespace whose link is throttled (tc `tbf`) and lossy (an `iptables` statistic rule;
  this host's kernel has no `netem`), and captured (tcpdump).

## M29: the derivation

Per participant, as ADR-0012 and the 1:1 measurement (`calls.md`, Capacity) have it: one
published camera and microphone at 0.752 Mbit/s on the wire. **Assumed layer: every subscriber
receives the top simulcast layer** (adaptive stream off, or every element large), which is the
most the SFU can send; a smaller layer only lowers egress. A room of n sends n x (n - 1) streams
and receives n (each publisher's layers, about 1.3 x the top one with simulcast):

| n | SFU egress | Rooms per SFU pod at 540 Mbit/s | CPU at 0.0079 cores per forwarded stream | Rooms per 2-core pod |
|---|---|---|---|---|
| 2 | 1.5 Mbit/s | 359 | 0.016 | 126 |
| 4 | 9.0 Mbit/s | 60 | 0.095 | 21 |
| 8 | 42.1 Mbit/s | 12 | 0.44 | 4 |
| 16 | 180 Mbit/s | 3 | 1.9 | 1 |

The cap of 8 keeps a full call under a quarter of a pod's CPU and under a tenth of its
bandwidth; 16 is the most one pod can carry at all. `tools/group_call_capacity_test.sh` runs
`tests/load/call_capacity` with rooms of n and checks the measured egress against
n x (n - 1) x the per-stream rate, within 10%. Measured locally (two rooms of four, 30 s, 700 kbps
per publisher, one layer): 8.96 Mbit/s out per call against the derived 8.4 (+6.7%, packet
headers included), out over in 2.96 against 3, and 0.047 cores per call, under the 0.095 the
1:1 figure extrapolates to; the CI runner's figure (`e2e.yml`) is the one to size by.

## Consequences

- A release with this needs migration 0015 applied first (the gateway's init container does).
- Expelling or removing one member reconnects everyone else in the call, once.
- After an owner change the new owner knows no call (ADR-0091): a member removed while nobody
  has asked the new owner for a ticket stays in the media room until they leave; the first ticket
  asked of the new owner starts a call it knows, and puts anyone removed out from then on.
- Group ring events cost one notice per member rung, every 15 s while ringing: up to 33 per
  call.
- Watch `call_moves_total`, `call_moves_failed_total`, `call_generations_closing` (old
  generations not closed yet: someone put out may still be connected) and
  `call_generations_abandoned_total`.
- Reopen when chat rooms get roles (admins may expel), when LiveKit's webhooks reach chat (end
  calls without polling), or when the SFU is replaced.
