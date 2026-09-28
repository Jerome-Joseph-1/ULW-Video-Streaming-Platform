# 0026. Accepted sockets use ordinary descriptors

Status: Accepted
Date: 2026-09-28

## Context

io_uring can accept into direct (registered) descriptors, which live only in the ring's file
table and not in the process descriptor table. An upload connection holds two descriptors: the
client socket and the backend socket to the object store, which libcurl owns. Only the client
socket could be direct, so direct descriptors would take an upload from 2 process descriptors to
1, roughly halving the per-connection cost.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Direct descriptors for accepted sockets | Halves process descriptors per upload; skips the file table lookup on each operation | Rejected: `getpeername` and `setsockopt` cannot take them, and neither can the epoll fallback or the libraries |
| Ordinary descriptors, with `RLIMIT_NOFILE` raised at start | Everything that takes a descriptor works on them | Accepted |

The rejection in detail:

- per-IP connection limits need the peer address from `getpeername`, which cannot be called on a
  direct descriptor;
- socket options (`TCP_NODELAY`, keepalive 60 s idle then 3 probes 10 s apart, `TCP_USER_TIMEOUT`
  20 s) would need `IORING_OP_URING_CMD` or a conversion back to an ordinary descriptor;
- the epoll fallback (ADR-0010) and libraries (libcurl, libpq, OpenSSL if it were ever bound to a
  descriptor) cannot use them at all.

## Decision

Accepted sockets are ordinary descriptors. `RLIMIT_NOFILE` is raised to 65,536 at start. The
descriptor budget is not binding: with 64 held back for listeners, the ring, signalfd and
database connections, (65,536 - 64) / 2 = 32,736 upload connections fit in the descriptor table,
against the 512 that admission control allows per process (ADR-0007), about 64 times headroom.

## Consequences

- Every accepted connection takes a process descriptor, and the per-operation file lookup that
  direct descriptors would skip stays.
- One code path for socket setup on both reactors.
- Monitor open descriptors per process against the limit.
- Reopen if descriptor count ever becomes the binding limit, for instance in a process holding
  tens of thousands of idle WebSockets.
