# 0010. io_uring is the primary reactor, epoll the fallback

Status: Accepted
Date: 2026-09-28
Supersedes: 0003

## Context

ADR-0003 built the loop on epoll, where every read and write is its own syscall. The upload
workload is many long-lived connections, each moving 64 KiB at a time. io_uring submits and reaps
many operations in one `io_uring_enter`, and a provided buffer ring lets the kernel choose a
buffer only when data arrives, so a connection waiting on a posted receive holds none. io_uring is
not available everywhere: Docker's default seccomp profile refuses `io_uring_setup`, and
`kernel.io_uring_disabled` can switch it off host-wide. Libraries (libcurl, libpq, signalfd) own
their descriptors and want readiness, not completions.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Stay on epoll (ADR-0003) | Runs everywhere; already written | Rejected as primary: one syscall per read and write; kept as the fallback |
| io_uring only | One code path; best syscall economy | Rejected: fails outright under Docker's default seccomp profile and wherever `kernel.io_uring_disabled` is set |
| io_uring behind the old readiness-shaped port | Keeps the ADR-0003 interface | Rejected: pays for io_uring's complexity and still reads with one syscall per readiness event |
| io_uring primary behind a completion-shaped port, epoll as a first-class fallback | Batching where the kernel allows it; still runs everywhere | Accepted |

## Decision

- The reactor port (`net::IReactor`) is completion-shaped. Mode A: the reactor owns the socket
  and hands the handler bytes it has already read; `stop_receiving` is exact. Mode B: `watch()`
  delivers readiness for descriptors a library owns (libcurl, libpq, signalfd), and the caller
  keeps ownership.
- The io_uring reactor is primary. `EpollReactor` is a first-class fallback and passes the
  identical parameterised test suite; a behaviour only one reactor has is a bug.
- `ULW_REACTOR=io_uring|epoll` selects the reactor, io_uring by default. If `io_uring_setup` fails
  or `kernel.io_uring_disabled` is 2, the process logs why and falls back to epoll.
- `tools/io_uring_probe.c` reports whether a host offers every io_uring feature the reactor needs.

## Consequences

- Two reactors to maintain. The shared suite is the contract between them.
- Mode B on io_uring re-arms a single-shot poll after each report, which gives libraries the
  level-triggered behaviour they expect.
- The reactor sets up its ring with `IORING_SETUP_DEFER_TASKRUN`, which needs kernel 6.1 or newer;
  older kernels run epoll.
- Monitor which reactor each process started with. A pod running under its container runtime's
  default seccomp profile may be refused io_uring and quietly run epoll.
- Reopen if io_uring is permanently unavailable where we deploy, making epoll the only path in
  practice.
