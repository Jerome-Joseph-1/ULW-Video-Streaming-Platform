# 0003. An epoll readiness reactor as the event loop

Status: Superseded by 0010
Date: 2026-09-28

## Context

The gateway needs one event loop per thread driving many nonblocking sockets, plus descriptors
owned by libraries: libcurl for the object store, libpq for Postgres, signalfd for shutdown.
Readiness is the interface all of them expect ("tell me when this descriptor is readable"), and
epoll is available on every Linux kernel and under every container seccomp profile we could be
deployed with. This was the first design of the loop.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| epoll readiness reactor | Universal; libraries integrate with readiness directly; simple to reason about | Accepted |
| io_uring completion reactor | One `io_uring_enter` submits and reaps many operations; fewer syscalls per byte | Rejected at the time: a newer kernel interface, refused by Docker's default seccomp profile, and a readiness interface was needed for libraries anyway |
| An event framework (Boost.Asio, libuv) | Mature, portable, already abstracts both kernel interfaces | Rejected: the gateway would adopt the framework's buffer ownership and threading model wholesale, while backpressure needs a small loop whose behaviour is exact |

## Decision

A single-threaded reactor built on epoll. Handlers are told a descriptor is ready and perform
their own nonblocking `read` and `write`. Library-owned descriptors register the same way.

## Consequences

- Every read and every write is its own syscall, so syscall cost grows with connection count
  and throughput.
- Backpressure is "stop asking for readability": unread bytes stay in the kernel's receive
  buffer.
- Reopen if syscall overhead dominates CPU under the upload workload.

Superseded by ADR-0010 when io_uring became the primary reactor. Readiness survives there as
"mode B", for descriptors that libraries own.
