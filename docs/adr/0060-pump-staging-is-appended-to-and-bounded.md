# 0060. Pump staging is appended to, bounded, and overflow is 413

Status: Accepted
Date: 2026-09-29

## Context

The gateway streams a PATCH body from the client's socket into the store (brief 8.4). When the
store's session takes fewer bytes than it was given, or has not been opened yet, the remainder is
held in a staging buffer and the connection stops receiving until the store can take it. Stopping
is not instant: the reactor has receives already in flight, and multishot completions that were
queued before `stop_receiving` still arrive afterwards. The parser too may hold a tail it has not
yet handed over. Bytes therefore reach `on_body` while staging is non-empty, and the question is
what to do with them.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Assign to staging each time bytes are refused | Simplest code; matches the common case of one refusal per call | Rejected: a second arrival while the first is still staged overwrites it and the upload silently loses bytes, and nothing notices until the object is read back |
| Write straight through even when staging is non-empty | No second path | Rejected: it reorders bytes, later data reaching the store ahead of earlier data |
| Append to staging while it is non-empty, in order, without limit | Never loses or reorders | Rejected: a client could grow it without bound while the store is stalled, which is the case backpressure exists for |
| Append while it is non-empty, drain in order, bound it, and fail the request when the bound is exceeded | Order kept, memory bounded, and a peer that ignores backpressure is refused | Accepted |

## Decision

- Bytes that arrive while staging is non-empty, or while the store's session does not exist yet,
  are appended behind what is staged, never written past it and never assigned over it
  (`Connection::on_body`, `apps/gateway/src/connection.cpp`). The remainder of a short write is
  appended the same way.
- `kMaxStaging` is 4 x 64 KiB = 256 KiB. Staging only ever holds what one parser callback handed
  over: one 64 KiB receive, or the parser's retained tail of at most 4 x 64 KiB. An append that
  would take the unsent staged bytes past it sets the request's body error to
  `Status::ContentTooLarge` (413) and pauses the parser.
- `drain_staging` writes from the head in order when the store is ready again, and the
  connection resumes reading only once it has emptied. ADR-0045 covers how long a request may
  wait while it is held.
- The comment beside the constant gives the derivation. The check is defensive: it cannot be
  reached from the wire today. Bytes that arrive while the connection is paused stay in the
  parser's retained tail (`retain()` in `http/src/request_parser.cpp`, capped at 4 x 64 KiB, past
  which the parser itself answers 413) and reach `on_body` only after `drain_staging` has emptied
  staging and resumed, so staging never holds more than one callback's worth.

## Consequences

- No byte is lost or reordered when completions keep arriving after receiving is stopped.
- A peer that keeps sending into a stalled store is held by the kernel window and the parser's
  bound, and costs its own connection (413), not memory.
- Code and brief agree: the append-or-413 rule is implemented as brief 8.4 describes it, with
  the bound spelt `4 * 64 * 1024`. One difference in wording only: the brief's step 1 checks
  the bound against the staged size plus the new data, and the code counts only the unsent part
  (`staging_.size() - staging_head_`), because a partly drained buffer keeps its consumed head
  until it empties.
- The observable bounds are pinned by tests: `Backpressure.BytesArrivingWhilePausedAreBounded`
  (the parser's retained tail) and `StoreHoldingTheBodyUpThrottlesTheClientWithoutTimingItOut`
  (at most 320 KiB ingested while the store is stalled). The connection's own 413 branch has no
  test because no input reaches it; exercising it would need the staging append moved into a
  type testable on its own.
