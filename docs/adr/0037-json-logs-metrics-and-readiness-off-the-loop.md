# 0037. JSON logs, metrics and readiness, none of them waiting on the loop

Status: Accepted
Date: 2026-09-29

## Context

Brief 8.14 asks for JSON lines on stdout, logged asynchronously through a bounded queue whose
overflow drops and counts; a request id on every response and in the job row; Prometheus metrics
sharded per thread; `/healthz` while the loop is alive and `/readyz` only while the database and
the store answer, 503 during startup and drain. Nothing on the gateway's reactor thread may
block (brief 7), and a pipe write blocks as soon as journald or `kubectl logs` falls behind; a
database or store round trip blocks for as long as the network says.

The request id already existed: a UUIDv7 per request, sent as `X-Request-Id` and written into
`jobs.request_id` by the commit (migration 0001). What was missing was a log line carrying it.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Log with `println` from the loop | Simplest; the worker did it | Rejected for the gateway: a stalled reader stops the loop |
| A lock-free ring per producer thread | No lock on the hot path | Rejected: one reactor thread logs almost everything; a mutex held for one `memcpy` is not contended |
| A bounded double buffer and a writer thread | Callers copy one line and return; the thread does every `write()` | Accepted |
| A third-party logging library | Features | Rejected: a line is a fixed-size JSON builder and one `write()` |
| `/readyz` queries the database and the store itself | Always current | Rejected: a blocking call per probe on the loop, or an asynchronous one per probe from every kubelet |
| A probe thread every 5 s, `/readyz` reads its last answer | The loop reads two atomics | Accepted |
| `jobs_oldest_queued_seconds` from the worker | It owns the queue | Rejected, see below |

## Decision

**Logging.** `ops::Logger` builds each line in a 1 KiB stack buffer: `ts`, `level`, `svc`,
`event`, then typed fields. A field that does not fit, counted at its escaped worst case, is left
out and the line says `"truncated":true`, so a long database error costs its own detail, never
the line. Levels are `debug < info < warn < error`, from `log.level` / `ULW_LOG_LEVEL`.

- The gateway writes through `ops::AsyncLogSink`: writers append to a 256 KiB buffer under a
  mutex, a thread swaps it for its twin and writes that out. A full buffer drops the line and
  counts it in `log_messages_dropped_total`. 256 KiB is about 850 request lines, over a second of
  the busiest the gateway gets (448 uploads each finishing an 8 MiB chunk no faster than every
  0.67 s, with playlist reads besides). On exit it flushes for at most 2 s, then writes the
  total of dropped lines, if any, as one `log lines dropped` line to stderr: stdout's reader is
  what fell behind, and the metric is gone with the listener. The sink needs an eventfd to wake
  its thread from a wait on a stalled reader at exit; if it cannot have one the gateway refuses
  to start (exit 1) rather than risk an exit that never ends.
- The worker writes each line with one `write(2)`: its threads block by design, and a line of at
  most 1 KiB to a pipe is atomic.
- One `request` line per response: request id, method, route name, status, milliseconds, request
  body length. `info`, `warn` for a 5xx, `debug` for probes and scrapes. Never the target, a
  header, a body, a token or a signed URL: the fields are fixed at the call sites, and
  `GatewayLog.NoTokenHeaderSignedUrlOrBodyEverReachesTheLog` sends tokens by header and cookie,
  a forged token, a malformed request, a body and a title with markers, and a playlist of signed
  URLs through the gateway at `debug`, then looks for each of them in every line. Key set fetch
  failures log the HTTP status or the transfer error only. The worker's per-job line carries the
  request id that queued the job.
- Settings are logged one line each at startup, with where each came from and
  `ULW_DATABASE_URL` as `<redacted>`.

**Metrics.** Prometheus text 0.0.4, every family with HELP and TYPE, scraped by a test that
parses the exposition strictly and compares the family set with the list it documents. A
`Gateway` is one shard of one reactor thread and owns its counters and histograms outright;
the exposition sums shards, of which there is one today. What other threads count is atomic
where it lives: the S3 store's paging errors, the log's drops, the probe's gauges.

- From 8.14 with a source today: connections accepted, rejected by reason (capacity, socket,
  draining), current; uploads in flight; bytes ingested; `part_upload_duration_seconds` (a
  chunk's first byte to the store until all of it is durable); `backend_write_stall_seconds`
  (each wait of a body on a store that took nothing more); admission rejections; timeouts by
  kind; TLS handshakes in flight; `buffer_bytes_in_use` (staging and body buffers); `open_fds`;
  `jobs_oldest_queued_seconds`. Besides: `build_info{version,git_sha}`, `ready`,
  `dependency_up`, responses by class, `resident_memory_bytes`, `store_paging_errors_total`.
- `parts_orphaned_total` has no source until the upload reaper (M13) exists; the realtime
  families wait for chat.
- `jobs_oldest_queued_seconds` is exported by the gateway. It matters most when workers are
  down, scaled to zero or all busy for minutes in blocking transcodes, which is exactly when a
  worker cannot report it; the gateway runs whenever uploads can arrive, already serves
  `/metrics`, and asks the database once per probe anyway. Every replica reports the same value;
  aggregate with `max`.

**Health.** `/healthz` (and `/api/v1/healthz`) answers 200 from the loop: if it answers, the
loop turns. A probe thread asks the database for the oldest due job's age on a session of its
own (2 s statement timeout) and the store for `health/probe`, a key nothing writes, where
`NotFound` means it answered; it also samples `/proc/self/fd` and RSS. `/readyz` (and
`/api/v1/readyz`) is 503 before the first probe (`starting`), while draining, when no probe has
finished for 30 s (past a healthy probe's worst case), and with a dependency down, named in the
body. A dependency that has never answered is down at once; one that has is down after two
failed probes in a row. Every replica probes the same database and store, so readiness that
flipped on one failure would take them all out of the load balancer together on a blip: the
probe's session is replaced on the call after it breaks, so a database restart or failover
costs one failed probe even when the new server answers the next. Two in a row, at least 5 s
apart, is an outage, reported within 10 s. `dependency_up` shows the last probe's raw answer.
While the database does not answer, `jobs_oldest_queued_seconds` is NaN rather than its last
value. The probe stops when the drain begins, so a probe stuck on a dead dependency (at most
about 13 s) runs out during the drain and the exit stays inside the 45 s grace. Transitions
are logged once.

**Drain and the service manager.** SIGTERM: `/readyz` 503 and `STOPPING=1`, the listener closes,
idle connections close, requests in flight get up to 30 s, then exit 0. `READY=1` is sent once
the listener is up (readiness for traffic is `/readyz`'s), and `WATCHDOG=1` from the loop at half
`WATCHDOG_USEC`, so only a turning loop keeps the process alive. The protocol is one datagram to
`NOTIFY_SOCKET`; it is written directly rather than linking libsystemd.

## Consequences

- A log reader that stalls for more than about a second loses gateway lines, and says so in a
  metric, instead of freezing uploads.
- Readiness lags a dependency's failure or recovery by up to one probe interval, 5 s.
- Several replicas report the same `jobs_oldest_queued_seconds`; dashboards take the max.
- Adding a thread that counts means an atomic or a shard of its own, never a shared counter
  written from two threads.
