# 0102. The worker publishes segments from a bounded pool of threads, each keeping one connection to the store

Status: Accepted
Date: 2026-10-09

## Context

On stage, a 1 h 55 min video spent about 40 minutes publishing, at about 0.5 MB/s: the worker
uploaded roughly 3,450 HLS files to R2 after the encode had finished. Two things made it slow,
both visible in the code:

- **One file at a time.** `publish()` uploaded each segment only after the previous one had
  finished. A segment is a few hundred kilobytes to a few megabytes, so each PUT was mostly
  waiting for round trips, not sending bytes.
- **A new connection per file.** `curl::perform_upload` made an easy handle for every request
  and freed it afterwards, and with it the connection: every PUT paid a TCP handshake and a TLS
  1.3 handshake to R2 before its first byte. At R2's distance from the cluster that is most of
  the half second each file took.

What had to hold whatever changed:

- **Order.** A player that finds a playlist must find everything it names: segments first, then
  each rendition's `index.m3u8`, then `master.m3u8`, the commit point (ADR-0024).
- **The lease.** A worker that loses its lease must stop writing within one request, as the
  sequential loop does: the job's next holder may be publishing the same keys (ADR-0006).
- **Retries.** Every attempt is signed afresh, so a retry never replays a stale `x-amz-date`.
- **Shared state.** Whatever the uploading threads share must be safe across them.
- **Operators who want today's behaviour** must be able to get it back exactly.

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| A bounded pool of threads for the segments, each with a handle kept for the whole publish | Overlaps round trips and keeps TLS per thread; one thread is today's loop; the playlists keep their order | Threads in the worker; more concurrent PUTs per worker | Accepted |
| Keep connections, upload one at a time | No threads | Still one round trip per file; about 3,450 of them is still minutes | Rejected: half the cure |
| libcurl's multi interface on one thread | No threads; libcurl schedules the transfers | The blocking transfer port, its tests and the worker's error handling all assume a call per file; a second upload engine to maintain beside the gateway's | Rejected for now |
| A share handle (`CURLSH`) of connections across every handle | Any handle reuses any connection | Locking callbacks around libcurl's connection cache, for what a handle per thread gives without them | Rejected |
| Concatenate segments into fewer, larger objects (byte-range HLS) | Far fewer requests | Changes the published layout, players' requests and the purge; every existing video differs | Rejected |
| HTTP/2 multiplexing | Many streams on one connection | One flow-control window shared by every upload; the curl layer pins HTTP/1.1 for that reason | Rejected |

## Decision

- **`curl::Session`** keeps one easy handle between blocking requests. Each request configures
  it afresh: the TLS floor, every option and the header list are set again as for a new handle,
  and `curl_easy_reset` clears them after the exchange. A reset leaves libcurl's connection
  cache, and the TLS session in it, alive. A Session is not safe from two threads at once.
- **`S3Transfer::put` uploads on a thread-local Session**, so each thread that uploads keeps one
  connection to the bucket for as long as it lives: the worker's job thread across jobs, a pool
  thread for one publish. Signing, the retry loop and its backoff are unchanged.
- **The publish pool.** The worker lists every rung's init and media segments first, then
  uploads them from `ULW_PUBLISH_CONCURRENCY` threads (1 to 32, default 8), the job's own thread
  among them, each taking the next segment not yet taken. Once every segment is up, the
  rendition playlists go up one by one, then the master. With 1 the pool is the sequential loop,
  in the same order as before.
- **The first failure stops the pool.** No thread takes another segment after it; the ones
  already sending finish that request. The attempt fails as a failed publish did before
  (retryable), and every key is fixed, so the next attempt overwrites whatever went up.
- **The lease.** Each upload first checks whether the keeper has lost the lease and fails if so,
  which stops the pool as any failure does: within one request per thread. A shutdown still lets
  a publish already under way finish, as before.
- **What the threads share is safe across threads.** `Endpoint::sign` is documented safe from the
  reactor and the pool at once; `RetryPolicy::next_delay` is const and reads only its
  configuration; `SystemRandom` calls `getrandom`; `PageCount` is atomic; the credential
  provider's values are fixed at startup. The worker's own counters (files and bytes published)
  are atomic, and the progress the pool reports only rises (`LeaseKeeper::raise`).
- **Visible.** Each publish logs `publishing` (`files`, `concurrency`) and `published` (`files`,
  `bytes`, `wall_ms`, `mib_per_s`), and progress moves from 90 to 99 a file at a time
  (ADR-0101).
- **8 by default.** Eight concurrent PUTs per worker are far inside R2's and S3's request
  rates, and enough to fill a node's uplink with segments of a few megabytes at R2's round
  trip. 32 is the cap: past it a worker's threads and connections grow without the uplink
  growing.

## Consequences

- Publishing a long video is bounded by the uplink instead of by round trips: for the stage
  video, from about 40 minutes to an expected 1-3, to be measured on stage with the `published`
  log line.
- Up to `ULW_PUBLISH_CONCURRENCY` connections per worker to the bucket while it publishes, and
  as many threads; more concurrent requests per worker against the store's rate limits, which
  operators can lower to 1.
- Segments arrive out of order; nothing reads them until a playlist names them, and the
  playlists still go up last.
- A connection kept between jobs may have been closed by the store meanwhile; libcurl detects a
  dead cached connection and opens a new one, and the retry policy covers what it does not.
- The gateway's S3 calls are unchanged: they run on its offload pool through `perform` and the
  multi interface, a handle per request.
