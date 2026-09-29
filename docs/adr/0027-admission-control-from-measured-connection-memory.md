# 0027. Admission control from the measured memory of a connection

Status: Accepted
Date: 2026-09-29
Supersedes: 0007

## Context

ADR-0007 derived the upload limit from an estimate of what one upload connection holds, about
348 KB: about 100 KB in the process (a ~200 B session, the 64 KiB pump buffer, ~35 KB of TLS
state) and the rest in kernel socket buffers and the backend socket. It asked to be reopened if
the process was far off that.

`ulw_gateway_load` now measures the in-process part. The gateway runs 500 uploads whose clients
live in a forked process, so its RSS is its own. RSS is sampled with every client connected, with
every upload stalled on a store that takes nothing, and while the store takes 16 KiB of each
upload every 100 ms and wakes it, so that every connection pauses and resumes at least 20 times;
VmHWM gives the peak. Peak RSS per connection, highest of three runs, kernel 6.18, after the two
fixes the measurement led to (the read BIO is kept instead of regrown after each backlog, and the
body of a receive is held in one buffer, not split between the parser and the pump):

| Reactor | Plain | TLS |
|---|---|---|
| epoll | 109.6 KB | 148.8 KB |
| io_uring | 155.2 KB | 203.9 KB |
| io_uring, less its 16 MiB receive ring (ADR-0021) shared by the 500 | 121.6 KB | 170.4 KB |

A plain connection with its client connected and idle costs 33.4 KB. Per term, on io_uring:
session and parser 33.4 KB, pump 121.6 - 33.4 = 88 KB (76 KB on epoll), TLS 170.4 - 121.6 =
49 KB (39 KB on epoll). The session estimate missed the request parser, whose target and header
buffers alone are 8 KiB + 16 KiB. The pump is larger on io_uring because a receive that completes
after `stop_receiving` is parked in the reactor (ADR-0021), so a connection can hold one staged
receive and one parked. The process holds 170 KB per TLS upload where ADR-0007 budgeted 100 KB.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Keep 512 from the estimate | Nothing changes | Rejected: 512 x 428 KB = 219 MB is 2.7 times inside 600 MB, not the 3 the safety factor promised; the measurement spent part of the margin meant for estimates and fragmentation |
| Shrink the parser's head buffers until 512 fits again | Keeps the limit | Rejected: they are the 8 KiB target and 16 KiB header limits, and the request's views point into them until it ends |
| Recompute the limit from the measured terms | The budget describes what the process holds | Accepted |

## Decision

An upload connection is budgeted as follows, each in-process term the measured value rounded up:

```
session and request parser            ~36 KB
pump: body staged, receive parked     ~92 KB
TLS state and parked ciphertext       ~52 KB
kernel receive buffer                128 KiB
kernel send buffer                    16 KiB
backend socket to the object store  ~100 KB
--------------------------------------------
per upload connection               ~428 KB
```

600 MB / 428 KB = 1,401 connections. Divided by 3 for safety, 467, rounded down to a multiple of
64 as ADR-0007 rounded to 512: 448.

- `max_connections` and `max_upload_slots` are 448 per process.
- A slot is acquired after routing and authentication, and only for upload chunks (`PATCH`).
  Unauthenticated requests, `HEAD` and playlist reads never take one.
- Over the limit the gateway answers `503 Service Unavailable` with `Retry-After: 5`.
- One user holds at most 3 slots at once: 448 / 3 means at least 149 users can upload
  concurrently.
- A slot is released only in the session destructor, so no path can release it twice or leak it,
  and it is not reused until the memory it stands for is freed.
- `ulw_gateway_load` holds the gateway's RSS at the stalled, slow and peak samples to n times the
  in-process terms (180 KB over TLS, 128 KB plain) plus the reactor's shared receive buffers.

## Consequences

- 64 fewer uploads per process than ADR-0007 allowed; capacity still grows by adding replicas.
- The in-process terms are measured averages, not bounds: a connection whose stop raced a receive
  on io_uring can hold 128 KiB of body. The factor of 3 covers that, allocator fragmentation and
  the connections that never take a slot. The kernel and backend terms are still estimates.
- ADR-0001's 35 KB for TLS state is replaced by the 52 KB here, which includes ciphertext parked
  below a paused protocol. ADR-0026's descriptor headroom becomes 32,736 / 448, about 73 times.
- Any change to a term (parser limits, pump, reactor receive size, TLS library, backend HTTP
  client) means rerunning `ulw_gateway_load` at 500 uploads on both reactors and both transports
  and recomputing the limit.
- Monitor slots in use, 503s issued, and RSS at peak. At 448 active uploads the gateway's RSS
  attributable to connections should be near 448 x 180 KB = 81 MB plus the 16 MiB receive ring;
  reopen if it is far off in either direction.
