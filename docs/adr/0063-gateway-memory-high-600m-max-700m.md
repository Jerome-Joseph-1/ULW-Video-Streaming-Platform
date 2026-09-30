# 0063. The gateway's memory: high at 600 MB, max at 700 MB

Status: Accepted
Date: 2026-09-29

## Context

The gateway runs on a 1 GB box (brief 8.1). ADR-0027 derives how many uploads it admits from a
per-connection cost and a budget for connections; ADR-0042 judges the soak against the same
ceiling. Neither states the limits systemd enforces, and the document the units were first
written from listed `MemoryHigh` above `MemoryMax`. `MemoryHigh` throttles a cgroup and
reclaims from it; `MemoryMax` kills it. A high mark above the max is never reached.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| `MemoryHigh` above `MemoryMax`, as in the source document | Matches what was written down | Rejected: the kernel never throttles, only kills |
| No `MemoryHigh`, only `MemoryMax` | One number | Rejected: nothing slows a process as it approaches the kill, so the first sign is the kill |
| `MemoryHigh` at the connection budget, `MemoryMax` 100 MB above it | Pressure starts at the point the derivation says the process should never reach, the kill leaves the OS its share | Accepted |

## Decision

- Box: 1 GB, less 150 MB for the OS, 50 MB for the binary and arenas, and a 200 MB margin,
  leaves 600 MB for connections (brief 8.1).
- `MemoryHigh=600M` and `MemoryMax=700M`, with `MemorySwapMax=0`, in
  `deploy/systemd/ulw-gateway.service`. The 100 MB between them is room for the kernel to throttle and
  reclaim before the kill, and it comes out of the 200 MB margin; the brief states the two
  numbers without deriving the 100.
- The Kubernetes deployments carry the same two numbers: the memory request is 600Mi and the
  limit 700Mi (`deploy/askedin/overlays/{stage,prod}/video-gateway/deployment.yaml`), so the
  kernel kills the pod where it would have killed the unit.
- The worker's unit sets its own: `MemoryHigh=1800M`, `MemoryMax=2G`
  (`deploy/systemd/ulw-worker.service`), from about 200 MB per rendition in parallel.

## Consequences

- Reaching 600 MB means the connection budget was wrong, and shows as throttling before it shows
  as a restart. The soak (ADR-0042) measures a leak against `MemoryHigh` for that reason.
- Code and brief agree on the numbers; this record adds the reasoning and the pod
  correspondence.
- Reopen if measured per-connection cost (ADR-0027) rises enough that 600 MB no longer holds
  the admission limit with its safety factor.
