# 0038. Soak flatness judged per unit of work against the memory limit

Status: Accepted
Date: 2026-09-29

## Context

M12 is done when a soak of 6 h shows flat RSS and descriptor counts for the gateway and the
worker. "Flat" needs a definition that a leak fails and allocator noise passes, stated before
the numbers are in.

A first definition judged the RSS slope per hour against the memory limit over 30 days. It
passed a 2 h run at about 5 requests a second, but a bound per hour at soak load says little
about production: at 0.78 MB/h and 18,500 requests an hour it admits about 40 bytes leaked per
request, which at the gateway's ceiling of 670 requests a second reaches `MemoryHigh` in under
a day. That load also left out whole paths: TLS, the body and header timers, a full admission
limit, a store that loses an object, a certificate reload.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Last sample within x% of the first | Simple | Rejected: the first samples include the warm-up, and one sample is noise |
| RSS slope per hour against MemoryHigh over 30 days | Uses every sample | Replaced: not normalised, it scales with the soak's load, not production's |
| Slope per unit of work (request, upload session, job), its 95% upper end against production rates | A leak is per something; the bound then holds at any load, and a run too short to tell fails | Accepted |
| No growth at all | Strict | Rejected for RSS: an allocator settling over hours is not a leak |

## Decision

`tests/soak/soak.py` runs both binaries against the local Postgres and MinIO, the gateway on
TLS, with eight clients making about 30 requests a second between them (playlists, video
status, upload offsets, nine kinds of bad request), one upload session every 3 s (a committed
MP4 in four, the rest resumed after a cut or cancelled), a slow client every 20 s (header, body
idle and body rate timeouts in turn), every upload slot filled once a minute to draw the 429
and 503 of both admission limits, a playlist deleted from the store every two minutes and then
fetched, and a SIGHUP every ten. It samples VmRSS and `/proc/<pid>/fd` of each process every
minute, excludes the first 15 minutes and fits a least-squares line to the rest.

- RSS: the upper end of the slope's 95% confidence interval, divided by the rate of each unit
  of work, must stay below (MemoryHigh - peak RSS) / (production rate x 720 h), so that even at
  the ceiling the process would not reach `MemoryHigh` within 30 days, the longest a replica
  runs between deploys. MemoryHigh is 600 MB for the gateway (brief, decision 11) and 1800 MB
  for the worker. The production rates are ceilings:
  - requests, 670 a second: 448 upload slots (ADR-0027) each sending an 8 MiB chunk every
    0.67 s, which 100 Mbit/s takes; everything else is small beside that. About 0.32 bytes per
    request: one 32-byte allocation in every hundred requests.
  - upload sessions, 5.3 a second: a 100 MiB upload over 10 Mbit/s holds its slot 84 s, and
    448 / 84 is 5.3. About 41 bytes per session.
  - jobs, one a second per worker: the soak's clip takes the worker 0.6 s end to end, the least
    any job takes. About 690 bytes per job.
  Judging the upper end rather than the slope means noise cannot pass for flatness: a run too
  short or too quiet to resolve the bound fails, and says so.
- Descriptors are flat when the upper end of the fitted rise over the window is below what may
  legitimately be in flight at a sample: two per client and for the uploads thread (a
  connection and its backend socket), plus the slow clients (four at most) and the saturation
  holds (13) for the gateway; 16 for the worker (one job's sessions, files and pipes); or the
  range seen in the warm-up if larger.
- Both processes must also exit 0 on SIGTERM at the end.
- `--rejudge` reads a run's `samples.csv` under both this criterion and the one it replaced;
  `docs/operations/soak.md` records the runs.

## Consequences

- The request bound needs a few tens of requests a second for hours to resolve: the soak's
  default load for six. A two-hour run at five a second cannot show 0.32 bytes a request, and
  fails.
- Not driven: the JWKS fetch path and database outages, which unit and integration tests cover
  rather than time.
