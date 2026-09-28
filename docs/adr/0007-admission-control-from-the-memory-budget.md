# 0007. Admission control per route, derived from the memory budget

Status: Accepted
Date: 2026-09-28

## Context

gateway_server runs in at most 1 GB. Without a limit, a burst of uploads pushes it into the OOM
killer, which drops every connection at once, including uploads that were about to finish. About
600 MB of the 1 GB is usable for connection state once the process baseline and headroom are set
aside. An upload connection holds memory on both sides of the pump:

```
session object                        ~200 B
pump buffer                           64 KiB
TLS state (ADR-0001)                  ~35 KB
kernel receive buffer                128 KiB
kernel send buffer                    16 KiB
backend socket to the object store   ~100 KB
--------------------------------------------
per upload connection                ~348 KB
```

600 MB / 348 KB = 1,724 connections. Divided by 3 for safety, 574, rounded down to 512. The
factor of 3 covers the estimated terms (TLS, backend socket), allocator fragmentation, and the
connections that never take a slot.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| No limit; rely on the kernel and the OOM killer | Nothing to tune | Rejected: an OOM kill drops every in-flight upload at once |
| One global connection limit at accept | One counter, cheapest check | Rejected: charges a playlist GET like an upload and refuses before knowing who is asking or what the request costs |
| A limit driven by measured RSS | Adapts to real usage | Rejected: RSS rises after allocation, so a burst is admitted before the reading moves |
| Per-route slots derived from the budget, taken after routing and auth | Charges only requests that hold the expensive resources | Accepted |

## Decision

- `max_connections` is 512 upload slots per process, from the derivation above.
- A slot is acquired after routing and authentication, and only for upload chunks (`PATCH`).
  Unauthenticated requests, `HEAD` and playlist reads never take one.
- Over the limit the gateway answers `503 Service Unavailable` with `Retry-After: 5`.
- One user holds at most 3 slots at once, so no single user can take the process: 512 / 3 means
  at least 170 users can upload concurrently.
- A slot is released only in the session destructor. No early return, error path or timeout can
  release it twice or leak it, and it is not reused until the memory it stands for is freed.

## Consequences

- The limit is per process. Capacity grows by adding gateway replicas, each with its own 1 GB.
- Any change to a term in the sum (pump buffer, socket buffer sizes, TLS library, backend HTTP
  client) means recomputing the limit.
- Monitor slots in use, 503s issued, and RSS at peak. At 512 active uploads RSS attributable to
  connections should be near 512 x 348 KB = 178 MB; reopen if it is far off in either direction.
