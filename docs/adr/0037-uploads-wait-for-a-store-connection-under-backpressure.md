# 0037. An upload waiting for a store connection is backpressured, not timed out

Status: Accepted
Date: 2026-09-29

## Context

A gateway process admits 448 uploads (ADR-0027). The S3 store sends every part through one
libcurl multi capped at 64 connections, and a part holds its connection from its first byte to
its last: an 8 MiB part at a client's 16 KiB/s holds one for 512 s. Under load most admitted
uploads therefore wait for a connection, and the wait for one is as long as the parts ahead of it
take.

A part beyond the cap waits in libcurl's queue of pending transfers. libcurl 8.5 runs no timer
against a pending transfer: it has no connection, so neither the connect timeout nor the
low-speed check applies, and parts set no overall timeout. While the part waits, its session's
64 KiB buffer fills, `write()` takes nothing more, the gateway stages what it already read and
stops reading the socket. That is the backpressure the pump is meant to apply (brief 8.4): the
client's bytes wait in its socket, not in the gateway.

The gateway's body timer read the same state as a failure. Its rule was that a body is stuck when
nothing has moved on either side of the pump for `body_idle_timeout` (30 s), and that staged bytes
at that point mean a stalled store, answered `503` and counted as
`timeouts_total{kind="backend"}`. A queued part cannot move, so every upload that waited 30 s for
a connection was failed.

A local run against a real gateway, Postgres and MinIO showed it (200 uploads of a 1.04 MB clip
at 16 KiB/s, `tests/load/upload_load.py`):

| | Before | After |
|---|---|---|
| `uploads_in_flight` | 200 until 34 s after admission, then 64 | 200 until the first parts finished, 66 s after admission |
| Uploads committed | 64 | 200 |
| Uploads failed | 136, answered `503` mid-body at 34 s | 0 |
| `timeouts_total{kind="backend"}` | 136 | none |
| Gateway RSS while uploads waited | 44.1 MB | 44.2 MB |
| Gateway peak RSS | 58.9 MB | 61.6 MB |

34 s is 4 s to fill the 64 KiB session buffer at 16 KiB/s, plus the 30 s body timeout.

After the change, the bodies of the 136 waiting uploads sat in their sockets' receive queues
(16.9 MB in all, about 124 KiB each) while the gateway's RSS stayed flat. With 500 uploads the
process admitted 448, the 64 streaming held 384 waiting with 47.6 MB in their receive queues
and RSS flat at 72.9 MB, and all 448 were committed with no timeout of any kind.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Raise the connection cap to the 448 admitted | No part would ever queue | Rejected: the cap also bounds descriptors and what one process asks of the store at once, and any later change to either number would bring the failure back |
| Lower admission to the 64 connections | Every admitted upload would have a connection | Rejected: a waiting upload costs only the memory ADR-0027 already budgets for it, so this would turn away seven in eight uploads the process can hold |
| A queue of our own in the S3 store, ahead of libcurl's | The store could tell a waiting part from a stalled one | Rejected: libcurl's queue is already first come first served and holds nothing but the transfer; a second queue changes nothing the gateway can see through the storage port |
| Tell the gateway, through the storage port, that a session is waiting rather than stalled | The gateway could keep its store timer for real stalls | Rejected: the store already ends a stalled connection itself, so the timer would only ever fire on waits that are fine; and the port would grow a call for a distinction nothing needs |
| The gateway leaves a body the store holds up to the store | The store is the one party that can tell a busy store from a broken one | Accepted |

## Decision

- While the store holds a body up (bytes are staged because the session took no more), the
  gateway runs no timer against the request but the six-hour backstop. The idle timer and the
  minimum body rate apply only while the gateway is reading, as the rate already did. Once the
  store takes bytes again the gateway resumes reading, and both start afresh.
- The store ends a wait that goes wrong. A part with a connection fails after 3 s without a
  connection being made, or after 60 s below 1 byte a second (`LOW_SPEED`); its session fails
  and the client gets `503` from the storage error, as before. A part without a connection
  waits.
- Parts wait in libcurl's queue, first come first served. They take no slot of their own and
  admission does not count them apart: a waiting upload holds its 64 KiB session buffer and its
  kernel receive buffer, both inside ADR-0027's per-upload budget, and no backend socket, which
  that budget counts for every upload anyway.
- `timeouts_total{kind="backend"}` is removed.

The wait needs no deadline of its own. At most 448 - 64 = 384 parts wait. A part holding a
connection while the gateway reads its client must average 8 KiB/s over every 30 s window, so
an 8 MiB part lets go of its connection within 8 MiB / 8 KiB/s = 1,024 s, plus one 30 s window
before a slower one is cut. A waiting part is behind at most 384 / 64 = 6 rounds of those:
6 x 1,054 s = 6,324 s, about 1.8 h, inside the six-hour request backstop that bounds every
request.

## Consequences

- An admitted upload is never failed for waiting. Its client sees a slow request, not an error,
  and a client with a request timeout shorter than the wait under full load (1.8 h at worst)
  gives up on its own and resumes from the durable offset.
- A store that stalls on a connection it holds is found after 60 s by libcurl, where the gateway
  found it after 30 s.
- A client that leaves while its upload waits is noticed only when the upload reaches the front
  and the gateway reads its socket again. Until then it keeps its admission slot.
- Whatever shares the store's multi must be able to wait behind uploads; the gateway's key
  fetches keep a multi of their own for that reason.
- A store that takes nothing and never says so again (the in-memory store's `accept_zero` fault)
  holds an upload until the backstop.
- `Multi::create` takes the connection cap, 64 unless a test asks for fewer; the gateway's
  integration test queues six uploads behind two connections.
