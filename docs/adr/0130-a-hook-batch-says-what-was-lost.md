# 0130. A hook batch says what was lost

Status: Accepted
Date: 2026-10-19
Amends: ADR-0111 (each request names its run and sequence, and the events lost before it)

## Context

Hook events are at most once, and never sent again (ADR-0111). A receiver could not tell it had
missed any: a request that failed, or events a full queue dropped, left no trace on its side
(#223).

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| Each request says its node run, a sequence number and how many events were lost before it | Gaps are visible at once, at no cost, and need no state beyond the sender's memory | Lost events are known, not recovered | Accepted |
| A replay window: events kept, and `events_since` on the service API | Recovers what was lost | A durable log per node, kept in step with the queue, and a second delivery path to secure | Rejected for now: the receiver reconciles from `call_status` and the service listings |
| A global sequence across nodes | One counter to watch | A write per event to a shared counter | Rejected |

## Decision

- Each request's body is `{"run","seq","lost","events"}`. `run` is a random tag drawn at the
  node's start, so a restart begins a new sequence. `seq` counts the run's requests from 1, one
  per request taken off the queue, whether or not it arrives. `lost` is the events the run
  dropped since the previous request was taken: those a full queue refused, and those of a
  request that failed.
- Nothing else changes: still at most once, still one request at a time per node.

## Consequences

- A receiver sees a gap in `seq` for a request that never came, and `lost` for what never left.
  It can then reconcile, or tell the user.
- A receiver that reads only `events` is unaffected.
