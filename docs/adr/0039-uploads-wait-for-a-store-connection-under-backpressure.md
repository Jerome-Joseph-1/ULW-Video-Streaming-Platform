# 0039. One store connection per admitted upload, and a held body is the store's to end

Status: Accepted
Date: 2026-09-29

## Context

A gateway process admits 448 uploads (ADR-0027). The S3 store sent every part through one
libcurl multi capped at 64 connections (brief 8.8), and a part holds its connection from its first byte to
its last: an 8 MiB part at a client's 16 KiB/s holds one for 512 s. Under load most admitted
uploads therefore wait for a connection, and the wait for one is as long as the parts ahead of it
take.

A part beyond the cap waited in libcurl's queue of pending transfers. libcurl 8.5 runs no timer
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

| | Before | After the timer change, still at 64 connections |
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
| Keep 64 connections and let parts queue, with no timer against a queued part | The smallest change; nothing admitted is failed for waiting | Rejected: 64 slow but legal clients hold every connection. At just over the 8 KiB/s floor an 8 MiB part keeps its connection for 1,054 s, 22 accounts of 3 uploads are enough, and every other upload waits behind them (measured below: 179 s, the whole run) |
| Keep 64 and the old 30 s rule | Queued uploads are at least cleared | Rejected: that is the defect; every upload that waits 30 s for a connection gets `503` |
| Keep 64 and bound each part tighter (a per-part deadline, or a higher floor while parts queue) | Keeps brief 8.8's number | Rejected: either floor fails honest slow links that the 8 KiB/s floor was derived to serve (ADR-0027 via `gateway.hpp`), and 64 clients at the tighter floor still hold every connection |
| Lower admission to 64 | Every admitted upload has a connection | Rejected: turns away seven in eight uploads the process has the memory for |
| A queue or a "waiting" signal of our own in the store | The gateway could tell a waiting part from a stalled one | Rejected: with a connection per admitted upload nothing waits, so there is nothing to tell apart |
| One store connection per admitted upload, and the gateway leaves a body the store holds to the store | A slow client holds only its own connection; a busy or slow store is ended by the store, which is the one party that can tell | Accepted |

## Decision

- The gateway's store multi is capped at `max_upload_slots` (448), not the 64 of brief 8.8.
  A part exists only while its upload holds an admission slot, one part at a time, so no part
  ever waits in libcurl's queue; libcurl closes an idle cached connection to make room when the
  cap is reached. ADR-0027 already budgets a backend socket for every admitted upload and brief
  8.1 two descriptors, so the cap spends nothing the budget has not counted.
- While the store holds a body up (bytes are staged because the session took no more), the
  gateway runs no timer against the request but the six-hour backstop. The idle timer and the
  minimum body rate apply only while the gateway is reading, as the rate already did. Once the
  store takes bytes again the gateway resumes reading, and both start afresh.
- The store ends a wait that goes wrong. A part fails after 3 s without a connection being made,
  or after 60 s below 1 byte a second (`LOW_SPEED`, the multi's stall limit); its session fails
  and the client gets `503` from the storage error.
- `timeouts_total{kind="backend"}` is removed.
- Key-set fetches keep a multi of their own, capped at 8: one fetch per verifier at a time, with
  room for overlapping refreshes.

Measured with 448 uploads of the 1.04 MB clip at 16 KiB/s, all admitted at once, against a
MinIO on the same host over plain HTTP (the local MinIO has no TLS, so the TLS state of the
backend socket is not in these numbers):

| | 64 connections | 448 connections |
|---|---|---|
| Live connections to the store | 64 | 448 |
| Gateway descriptors | not sampled | 914: 2 per upload and 18 of its own |
| Gateway RSS while every upload was in flight | 72.9 MB | 78.3 MB |
| Uploads committed | 448 | 448 |

The 384 extra live connections cost 5.4 MB of RSS, 14 KB each, well inside ADR-0027's ~100 KB
backend-socket term, which also covers the kernel's buffers for it.

`tests/load/slowloris.py --mode upload` runs 64 uploads of an 8 MiB part at 9 KiB/s, just above
the floor, for 180 s, with an ordinary 1 MiB uploader beside them
(`legit_client.start_uploads`):

| Ordinary uploader | 64 connections | 448 connections |
|---|---|---|
| Uploads finished in 180 s | 2 | 172 |
| Slowest upload | 178.9 s, until the slow clients left | 407 ms |
| p50 / p99 | two samples | 38 ms / 221 ms |

How long a request can take, now that nothing queues. A part whose client is slow must average
8 KiB/s over every 30 s window, so it ends within 8 MiB / 8 KiB/s = 1,024 s plus one 30 s window
for a slower one to be cut: 1,054 s. A PATCH carries at most 16 MiB (the parser's limit), two
parts, so 2,108 s. A part the store is slow to take is bounded by the store's own limits and,
past them, by the six-hour backstop.

At 64 connections the same bound would have been much worse. A part could wait behind
(448 - 64) / 64 = 6 rounds of 1,054 s and then hold its own connection for another, 7,378 s;
the second part of a 16 MiB PATCH queues again, so 14,756 s, 4.1 h. That is inside the backstop
by a factor of 1.5 only, and it rests on libcurl 8.5 happening to promote pending transfers
first come first served, which it does not promise.

## Consequences

- An admitted upload never waits for a store connection, and a slow client costs other uploads
  nothing but its own slot.
- The process can open 448 connections to the store at once, not 64. The store's own per-client
  connection limits must allow that; the local MinIO did, and R2's is to be checked in the
  sandbox run.
- A store that stalls on a connection it holds is found after 60 s by libcurl, where the gateway
  found it after 30 s. `MultiTest.AnUploadThePeerStopsReadingFailsAsATimeoutAfterTheStallLimit`
  and `GatewayStoreStall.AStoreThatStopsReadingAPartFailsTheUploadWith503` hold that, with a
  one-second stall limit because libcurl's timer runs on its own clock.
- A client that leaves while the store holds its body up is noticed once the store takes bytes
  again and the gateway reads its socket.
- A store that takes nothing and never says so again (the in-memory store's `accept_zero` fault)
  holds an upload until the backstop.
- `Multi::create` takes the connection cap and the stall limit. The gateway's integration test
  still queues six uploads behind two connections, to hold the gateway to backpressure, not
  failure, whenever a store does hold a body up.
