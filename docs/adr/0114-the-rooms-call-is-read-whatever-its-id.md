# 0114. The room's call is read whatever its id, and the backend may end it for a member

Status: Accepted
Date: 2026-10-19
Amends: ADR-0111 (call_status may leave out the call; the service API cancels, ends and leaves a
call for a member as it declines one)

## Context

Three gaps were left once a call's lifetime followed its media room (ADR-0113):

- **After a reload** (#188) neither a client nor the backend could learn the room's call. The
  service API's `call_status` needed the call's id, which only hook events carry, and those are
  at most once. It said ringing, answered or over, never who was in it. A reloaded caller could
  not show "Calling…" again, and a reloaded callee could not show "Rejoin".
- **A page going away** (#189) can `navigator.sendBeacon` to its product's backend, but not
  reliably write on a WebSocket. The backend could decline a call for a member and nothing else,
  so a caller's cancel or a member's hang-up was lost with the page.
- **A socket that joined mid-ring** (#190) heard the ring at the next announcement, up to 15 s
  later. Meanwhile a ring that had ended kept a live Answer button on screen.

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| Replay hook events from a log | The backend keeps the ids it already gets | Hook events are at most once by design (ADR-0111), and a log is state chat does not keep | Rejected |
| The room's owner answers the room's call from what it holds, with the SFU's listing | One read, from memory and one SFU call; the same for the socket and the backend | Only while the owner holds the call (a change of owner still forgets it, #194) | Accepted |
| A per-user registry of ringing calls, replayed to every socket on upgrade | Covers a socket that joined no room | A write per ring event, and a table to keep in step with the owner's memory | Deferred to #194, which needs ringing state outside the owner anyway |

## Decision

- **A current-call ask** (`CallCurrentRequest`, kind 8 of the call ask layout) reads the room's
  call from its owner. The owner answers `Current` with a `CallSnapshot`:
  - the call's id, kind and caller, and whether it is answered;
  - who it still rings and who holds a ticket in it;
  - who the SFU lists in its media room, or nothing when the SFU cannot say;
  - when it started, was first answered, and stops ringing.

  A room with no call is `NoCall`. No member list is read, as for the status ask (ADR-0111).
- **The socket** asks it with `call_status` (`room`), on a room it joined, charged as a join.
  The service API asks it with `call_status` and `room` alone. Both answer the same frame, and
  `call: null, status: "none"` for a room with no call.
- **Every join** of a direct or group room asks it, uncharged. When the answer says the call
  rings the socket's user (a callee still rung, or the caller while nobody has come), the socket
  is sent `call_ringing` at once. A stream's live chat is never asked.
- **The service API** gains `cancel_call`, `end_call` and `leave_call` (`room`, `call`,
  `user`). Each is the member's own signal, as `decline_call` is: checked against the member
  list on the owner, allowed only in the state the socket's would be, and answered with the
  event everyone hears.

## Consequences

- A join of a direct or group room costs one more owner ask, answered from memory on the owner.
- A socket that joined no room still hears a ring at the next announcement. A replay on the
  socket's upgrade needs the rings of every room the user is in, which no node holds. That is
  left for when ringing state outlives its owner (#194).
- The call answer layout gains outcome 14. A node of the previous release reads it as malformed
  and answers `unavailable`. A `Recreate` rollout (ADR-0112) has no such window.
