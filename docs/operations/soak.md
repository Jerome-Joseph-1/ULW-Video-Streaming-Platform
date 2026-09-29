# Soak runs

`tests/soak/soak.py` runs `gateway_server` and `transcode_worker` against the local Postgres and
MinIO for hours, samples each process's RSS and open descriptors every minute into
`samples.csv`, and judges whether both stayed flat. The criterion, and why, is ADR-0042.

```
docker compose -f deploy/local/compose.yaml up -d --wait
cmake --preset ci && cmake --build --preset ci
tests/soak/soak.py --build build/ci --hours 6 --out /tmp/soak-6h          # a run
tests/soak/soak.py --rejudge /tmp/soak-6h/samples.csv --clients 8        # judge it again
```

`--rejudge` prints two verdicts: the per-unit criterion in force (the RSS slope's 95% upper end
per request, per chunk request, per upload session and per job, against production ceilings),
and the per-hour criterion it replaced. Pass `--clients` as the run used it; it sets the
descriptor bound.

The job ceiling is 1.67 a second (the clip's 0.6 s end to end, the least a job takes), which
bounds the worker at about 413 bytes a job; the runs below were judged again with it and their
verdicts did not change. Chunk requests are counted from the soak that follows these runs; the
three runs here predate the column, so for them `--rejudge` reports the per-chunk line as not
judged, and a leak on each chunk shows only through the per-session line, since every chunk
belongs to a session.

## Runs

The first two runs used the load and criterion of the soak's first version: four clients on
plain HTTP at about 5 requests a second (playlists, bad requests, uploads, resumes,
cancellations), and RSS judged by its slope per hour. Both are re-judged below under the
current criterion. Their binaries were built from 6859bb9, the branch before its rebase onto
d33539e; the rebase changed no code the soak exercises (it merged the worker's heartbeat file).

### 2 h, 2026-09-29 04:40 to 06:46 UTC

A two-hour run, recorded as one. 126 samples; judged window 111 samples over 1.83 h.
35,266 2xx, 3,391 4xx, no 5xx, no transport errors: 586 uploads committed,
1,181 resumes, 1,210 cancellations, 8,114 playlists, 3,883 bad requests. Both processes exited
0 on SIGTERM.

| | first | last | peak | slope | 95% upper end |
|---|---|---|---|---|---|
| gateway RSS | 36.5 MiB | 36.5 MiB | 36.6 MiB | +12.2 KB/h | +52.3 KB/h |
| worker RSS | 14.8 MiB | 14.8 MiB | 14.8 MiB | 0 | 0 |
| gateway fds | 25 | 25 | 25 (min 23) | -0.07/h | rise at most +0.29 |
| worker fds | 4 | 4 | 9 (min 4) | +0.07/h | rise at most +0.66 |

- Per-hour criterion (replaced): **pass**. Gateway 0.012 MB/h against 0.780; worker 0 against
  2.478; descriptors flat.
- Per-unit criterion (current): **fail, not resolved**. At 18,507 requests an hour the gateway's
  upper end is 2.83 bytes a request against a bound of 0.32: two hours at five requests a
  second cannot resolve it. Per upload session 37.1 bytes against 40.9, flat; worker 0 bytes a
  job against 413, flat; descriptors flat.

### 6 h, 2026-09-29 04:40 to 10:40 UTC

Started beside the 2 h run on the same binaries and load. 360 samples; judged window 345
samples over 5.73 h. 99,947 2xx, 9,207 4xx, 10 5xx, no transport errors: 1,723 uploads
committed, 3,363 resumes, 3,364 cancellations, 22,952 playlists, 10,824 bad
requests. Both processes exited 0 on SIGTERM.

The ten 5xx were all between 10:12:43 and 10:12:55 UTC, while the shared Postgres container had
been left paused by another suite's test (`docker pause`) and was then resumed; they are that
outage answered as 503, not the soak's doing.

`videos_ready` in the samples stays at 200 from minute 118: it is the size of the working set
of ready videos the clients play from, capped at 200 by design, not a count of what the worker
finished. `uploads_committed` (1,723) counts the commits.

| | first | last | peak | slope | 95% upper end |
|---|---|---|---|---|---|
| gateway RSS | 36.5 MiB | 36.1 MiB | 37.0 MiB | -83.2 KB/h | -67.9 KB/h |
| worker RSS | 14.8 MiB | 13.9 MiB | 14.8 MiB | -168.4 KB/h | -154.2 KB/h |
| gateway fds | 25 | 25 | 29 (min 22) | +0.06/h | rise at most +0.69 |
| worker fds | 4 | 4 | 9 (min 4) | +0.08/h | rise at most +0.89 |

- Per-hour criterion (replaced): **pass**.
- Per-unit criterion (current): **pass**. The upper ends are negative: RSS fell slightly over the
  window. Gateway at most -3.7 bytes a request (bound 0.32) and -48 a session (bound 40.9);
  worker at most -536 bytes a job (bound 413); descriptors flat against bounds of 27 and 16.

This run lacked the paths the current load adds: TLS, the header, body idle and body rate
timers, full admission limits, store faults and certificate reloads.

### 6 h on the current load, started 2026-09-29 10:41 UTC

Binaries from ee075b6, the current soak load (eight clients on TLS, about 30 requests a second,
with slow clients, saturation, store faults and SIGHUPs) and criterion.

Run 3 (full load) in progress; recorded when it ends at about 16:41 UTC.
