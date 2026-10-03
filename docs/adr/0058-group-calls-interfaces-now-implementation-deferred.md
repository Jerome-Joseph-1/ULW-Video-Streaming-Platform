# 0058. Group calls: interfaces now, implementation deferred

Status: Accepted, implemented by 0095
Date: 2026-09-29
Amends: ADR-0050 (adds one method to `IMediaRoom`)

## Context

Phase 5 of the plan is group calls: M27 (room-pinned group sessions), M28 (adaptation) and M29
(capacity). On 2026-09-29 the project owner parked them: the platform ships 1:1 calls (M23 to
M26) and the work on group calls waits. ADR-0050 already shaped the media port for them
(`open_room` takes a participant cap, expelling is a new generation, owner reassignment reuses
`open_room`), so what is left is small. Landing that part now keeps the port from being reshaped
later under a client that has come to depend on it.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Land nothing until group calls are resumed | No code for a feature nobody uses | Rejected: the port change is one method, and settling it now while ADR-0050 is fresh costs less than reopening the port later |
| Land the port and a real implementation | Group calls work | Rejected: the project owner's decision of 2026-09-29 |
| Land the port; adapters answer `NotImplemented` | Callers compile against the final shape and fail loudly | Accepted |

## Decision

- **The port.** `IMediaRoom::participants(ParticipantsDone)` returns the devices connected to a
  generation (`MediaParticipant`: user, device, join time). It is what M27's first criterion
  needs to know from the server side, that a room holds exactly the admitted members. Nothing
  else was added: room creation with a cap, roles, per-participant tickets and expulsion by
  generation are ADR-0050's and already there.
- **`MediaError::NotImplemented`.** The LiveKit adapter's `participants` reports it, later and
  without a request to LiveKit, like every other callback of the port. There is no in-memory
  adapter to update; the call suite and the unit tests drive the LiveKit adapter itself.
- **M28 needs no port.** LiveKit has no per-subscriber layer selection in its server API, so
  simulcast, transport-cc, NACK and PLI run between the browser SDK and LiveKit (ADR-0050). M28
  is client code shipped with the call page and measurements in tests/call.
- **Group calls are not offered.** `docs/integration/calls.md` lists them as planned, so
  Askedin's teams do not build against them.

## Deferred acceptance criteria

From the plan, unchanged, to be met by the follow-up:

M27 Room-pinned group sessions
- 4 peers each see 3 inbound SSRCs.
- Forced owner reassignment mid-call produces a bounded interruption followed by full recovery.
- The peers are spread across at least 2 signalling nodes.

M28 Adaptation
- A `netem`-throttled receiver downshifts layers within a measured time.
- Induced loss shows NACK -> retransmit in a pcap.
- PLI -> keyframe within a measured time.
- The bandwidth estimate converges to within a measured tolerance of the cap.

M29 Capacity
- The measured egress matches the derivation within +-10%.
- The assumed simulcast layer is stated explicitly.
- Tag `phase-5`.

## What a follow-up needs

- LiveKit's participant listing (RoomService `ListParticipants`, a Twirp call with a
  `roomAdmin` grant, like the calls the adapter makes today) behind `participants`, and a test
  that runs it against a real LiveKit.
- A group call handler: membership beyond two, `max_participants` chosen from M29's derivation,
  the room-pinned session on the owning node, and a generation move on every expulsion (which
  reconnects the whole room, ADR-0050).
- tests/call scenarios for four peers on two signalling nodes, an owner reassignment, and the
  M28 network conditions (`netem`, pcap capture).
- `tools/group_call_capacity_test.sh` and the derivation next to the cap.
- The integration guide's group section, written when the above exists.

## Consequences

- Any other `IMediaRoom` implementation must define `participants`; the adapter's answer is
  `NotImplemented` until the follow-up lands, and callers must treat it as final for the build.
- Nothing about group calls is promised to clients until this ADR is superseded.
