# 0129. A ring outlives its owner

Status: Accepted
Date: 2026-10-19
Amends: ADR-0091 (a ring whose room changes owner ends out loud); ADR-0114 (a socket hears its
user's rings as it connects)

## Context

A room's ringing call lived in its owner's memory alone. When the owner changed (a rolling
deploy, a node lost) the call was forgotten without a word (ADR-0091) (#194):

- devices rang on until `expires_at`;
- a decline or a cancel was `no_call`;
- an answer started a new call, which rang the caller;
- the hook heard nothing after `call_ringing`.

Every daytime deploy broke the calls ringing at that moment. A socket that connected also
waited up to 15 s for the next announcement of a call ringing its user, since a ring could
only be read from the room's owner, by a socket that had joined the room (#190, ADR-0114).

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| The owner keeps each ring in a table; a drain ends its rings out loud; a new owner ends what it finds; a socket reads its user's rings as it connects | Every ring ends with an event, deploy or crash; the same rows serve the upgrade; writes are two per ring | A ring is not carried on by the new owner: its members ring again | Accepted |
| The new owner carries the ring on from the table | Seamless | The ring's state (ringing, joined, expelled, answered devices, the media generation) is far more than a row, and must then be kept in step on every event | Rejected for now |
| The old owner cancels on drain only | No table | A crash stays silent, and the upgrade replay still needs the table | Rejected |

## Decision

- **The table.** Migration 0026 adds `chat_call_rings`: one row per room, written by the owner,
  best effort, when a ring starts. The row goes when the ring ends for everyone it rang (a
  direct call answered or declined, any call cancelled, missed or ended). A row is good until
  `expires_at`.
- **A drain** (SIGTERM) first ends every call that still rings, unanswered: `call_cancelled`
  with no `by` and `reason: "moved"` (CallReason::OwnerMoved, notice byte 5), to the members'
  sockets and the hook. Answered calls go on in their LiveKit rooms.
- **A new owner** (rt::IRouterEvents::on_took_room) reads the room's row. If the row still
  rings and this node holds no call there, it sends the same event for that call, and the row
  goes.
- **A socket that connects** reads the rings, not past `expires_at`, whose callees include its
  user (at most 32, the newest first). It is sent `call_ringing` for each at once.

## Consequences

- A deploy cancels the rings in flight out loud, with `reason: "moved"`, and their callers ring
  again; a crash does the same once a new owner takes each room.
- Each ring costs two writes. Each socket's upgrade costs one indexed read, with a GIN index on
  `callees`.
- A node of the previous release drops a cancelled notice with reason 5, as it drops any reason
  it does not know (ADR-0113). This release moves the node channel anyway (ADR-0128), so the
  `Recreate` rollout leaves no window.
- New counters: `call_rings_total{outcome="moved"}`, `call_ring_writes_failed_total`,
  `call_rings_replayed_total`.
