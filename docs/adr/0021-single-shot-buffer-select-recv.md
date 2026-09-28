# 0021. Single-shot buffer-select receives, not multishot

Status: Accepted
Date: 2026-09-28

## Context

The io_uring reactor (ADR-0010) receives into a provided buffer ring of 256 x 64 KiB buffers
(16 MiB) shared by all connections, so a connection holds no receive buffer while idle. The port
promises that `stop_receiving` is exact: no data is delivered after it until `start_receiving`.
The upload pump depends on that for backpressure and stages at most 4 x 64 KiB = 256 KiB per
connection. Multishot receive, one submission that keeps completing until cancelled, looks like
the natural partner for a buffer ring.

Measured on kernel 6.18 over loopback with the 256 x 64 KiB ring, with 3,919,467 bytes queued
in the socket before the receive was posted:

- one multishot receive posted 60 completions carrying all 3,919,467 bytes in a single batch.
  A `stop_receiving` issued on the first completion is followed by the other 59, carrying
  3,919,467 - 65,536 = 3,853,931 bytes, about 3.85 MB;
- one single-shot buffer-select receive delivered exactly one completion of 65,536 bytes.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Multishot receive with buffer select | One submission per connection for its lifetime; the fewest SQEs | Rejected: completions already posted cannot be taken back, so about 3.85 MB arrived after `stop_receiving`, about 15 times the pump's 256 KiB bound and 11 times the 348 KB per-connection budget (ADR-0007) |
| Multishot, with the excess parked until resume | Keeps multishot's submission economy | Rejected: the parked data is bounded only by the socket backlog, 3.85 MB per connection in the measurement |
| Single-shot receive into a per-connection buffer | Exact stop; no shared ring | Rejected: every connection pins 64 KiB for as long as a receive is posted, idle or not |
| Single-shot receive with buffer select | Exact stop; a buffer is chosen only when data arrives | Accepted |

## Decision

The io_uring reactor receives with single-shot `IORING_OP_RECV` and `IOSQE_BUFFER_SELECT`,
re-armed after each completion while the connection is receiving. This keeps the provided-buffer
memory model and makes `stop_receiving` exact on both reactors. A completion that races a stop is
parked, at most one 64 KiB buffer, and delivered on resume from the loop, never re-entrantly.
Multishot is still used for accept.

## Consequences

- One SQE per 64 KiB received, batched into the same `io_uring_enter` as the rest of the
  iteration. At the full 600 Mbit/s (75 MB/s) that is 75,000,000 / 65,536 = about 1,144 SQEs per
  second.
- When all 256 buffers are in use a receive completes with `ENOBUFS` and is re-armed on the next
  iteration. Monitor its rate; a steady stream means the ring is too small for the load.
- Reopen if the kernel offers a per-invocation cap on multishot receives that the kernels CI runs
  on support.
