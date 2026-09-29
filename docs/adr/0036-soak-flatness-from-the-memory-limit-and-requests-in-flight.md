# 0036. Soak flatness judged against the memory limit and requests in flight

Status: Accepted
Date: 2026-09-29

## Context

M12 is done when a soak of 6 h shows flat RSS and descriptor counts for the gateway and the
worker. "Flat" needs a definition that a leak fails and allocator noise passes, stated before
the numbers are in.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Last sample within x% of the first | Simple | Rejected: the first samples include the warm-up, and one sample is noise |
| A fitted slope below a bound derived from the deployment | Uses every sample; the bound means something operationally | Accepted |
| No growth at all | Strict | Rejected for RSS: an allocator settling over hours is not a leak |

## Decision

`tests/soak/soak.py` runs both binaries against the local Postgres and MinIO with four clients
at about two actions a second (playlists 55%, bad requests 25%, uploads 4%, resumes 8%,
cancellations 8%), samples VmRSS and `/proc/<pid>/fd` of each process every minute, excludes the
first 15 minutes, and fits a least-squares line to the rest.

- RSS is flat when the slope would not carry the process to its `MemoryHigh` within 30 days,
  the longest a replica runs between deploys: slope < (MemoryHigh - peak RSS) / 720 h, with
  MemoryHigh 600 MB for the gateway and 1800 MB for the worker.
- Descriptors are flat when the fitted rise over the window is below what may legitimately be
  in flight at a sample: 2 per client for the gateway (a connection and its backend socket),
  16 for the worker (one job's sessions, files and pipes), or the range seen in the warm-up if
  larger. A leak of one descriptor per thousand requests, at this load's ~5 requests a second,
  is 18 an hour and fails.
- Both processes must also exit 0 on SIGTERM at the end.

## Consequences

- A slow leak under a megabyte an hour passes this soak; the 30-day bound says it would not
  matter before the next deploy, and the gateway's `resident_memory_bytes` watches production.
