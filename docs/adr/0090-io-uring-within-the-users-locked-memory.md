# 0090. The io_uring reactor stays within its user's locked memory: zero copy only where the limit cannot refuse it

Status: Accepted
Date: 2026-10-03
Amends: ADR-0051 (zero-copy sends are tried only where RLIMIT_MEMLOCK is unlimited or the
process has CAP_IPC_LOCK); ADR-0086 (the integration label's steps run with the locked-memory
limit lifted, as #132 put it in ci.yml)

## Context

After ADR-0086 put the integration label on `ctest -j "$(nproc)"`, main's `integration (asan)`
job (run 37122448144, job 111201418211, commit e7897b4) failed
`Reactors/CatalogTest.ClaimReturnsTheUploadAndRefusesASecondGateway/IoUring` in its `SetUp`:
`make_reactor` returned ENOMEM ("reactor: Cannot allocate memory"). The runner image was
ubuntu-24.04 20260927.320, kernel 6.17.0-1022-azure; the tests run as the unprivileged `runner`.
At that moment the only other tests running were three of the same suite, one of them on
io_uring and starting up.

What the kernel does (io_uring/memmap.c, notif.h):

- Since 6.14 the SQ and CQ rings are regions (8078486e1d53 and 81a4058e0cd0, through
  `io_create_region` and `__io_account_mem`), and every region's pages are charged to
  `user->locked_vm` against the creating process's soft RLIMIT_MEMLOCK, failing with ENOMEM
  (5.12 to 6.13 charged them to the memory cgroup only). The counter is per user, shared by
  every process of that uid; only CAP_IPC_LOCK in the initial user namespace exempts a process.
- Every zero-copy send in flight charges `(len >> PAGE_SHIFT) + 2` pages to the same counter,
  given back when its notification is reaped: `SEND_ZC` since 6.0; `SENDMSG_ZC`, which the
  reactor uses, only since 6.15 (04491732fc99, not in the 6.6, 6.8, 6.12 or 6.14 stable trees).

The reactor's rings are 4,096 SQEs and 8,192 CQEs plus two provided buffer rings: 104 pages,
about 416 KiB. Measured on 6.18 as an unprivileged user under an 8 MiB limit: 20 bare rings fit
and the 21st is refused; 19 with the buffer rings. But `UringReactor::create` then probed zero
copy (ADR-0051) with 1,024 `SENDMSG_ZC` sends in flight at once, as many as the reactor ever
holds: 2,048 pages, 8 MiB, the whole of the default limit. Under it the probe alone sent 973 and
was refused 51 with ENOMEM. Each starting reactor so took its user's counter to the limit for a
few milliseconds, and any other process of the user creating a ring meanwhile was refused:

- a process creating a ring every 2 ms: 0 of 4,000 refused alone, 133 of 4,000 beside another
  process probing in a loop;
- the 36 io_uring tests of `postgres_integration_tests` (asan), twice each, four at a time as an
  unprivileged user under 8 MiB: one ENOMEM in 216 runs before, none in 432 after.

This does not explain ADR-0051's observation on the runners' 6.8 kernel, where a single
zero-copy send succeeded and some of a burst of 1,024 failed: upstream 6.8 charges neither the
rings nor `SENDMSG_ZC` to the limit. Unless Ubuntu's 6.8 backported 04491732fc99, those errors
remain unexplained, and the burst stays in the probe for them.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Lift the limit in CI only | One line in the workflow | Rejected alone (CI does lift it, below): production hosts with the 8 MiB default (Debian, Fedora, systemd services) would have each starting reactor refuse the others, a gateway's shards included, and fall back to epoll |
| Probe with one send, rely on the per-socket fallback | Keeps zero copy everywhere | Rejected: 1,024 zero-copy sends in flight still charge 8 MiB at run time, whatever the probe did |
| Probe in small batches | Lower peak during the probe | Rejected: same run-time charge, and the burst is what proves the kernel takes as many as the reactor holds |
| Try zero copy only where RLIMIT_MEMLOCK is unlimited; otherwise plain sends, no probe | The reactor charges its user its rings and nothing else under any finite limit; zero copy's benefit for datagrams of 2 KiB or less is unmeasured, and loopback always copies (ADR-0051) | Accepted |
| Smaller rings | Less per reactor | Rejected: not the cause; 4,096 entries are sized for 448 connections per shard |
| A finite limit in CI of 8 MiB per core | Keeps CI as close to a default host as `-j` allows | Rejected: the counter is the sum over the user, and a ring stays charged for a moment after its process exits, so no figure sized from one test holds |

## Decision

- `UringReactor::probe_zero_copy_send` returns false without sending anything unless the soft
  RLIMIT_MEMLOCK is RLIM_INFINITY or the process has CAP_IPC_LOCK effective in the initial user
  namespace (its uid_map maps every id to itself; inside another, the kernel ignores the
  capability). Then the kernel charges it nothing it can be refused, and the 1,024-send probe
  runs as before.
- `ReactorFactory.StartingAnIoUringReactorChargesTheUsersLockedMemoryOnlyItsRings` starts two
  reactors in a child with an 8 MiB limit and without CAP_IPC_LOCK, while another process of the
  same user, under 6 MiB of its own, registers and unregisters a 16 KiB buffer in a loop. A
  refusal means the user's charge passed 6 MiB while two reactors' rings are under 1 MiB. Run as
  root, the child first becomes a uid no account or process has, so its counter is its own; run
  as another user, a refusal before the reactors start (the user's other processes already hold
  the room) skips the test with that reason. With the probe as it was it fails most runs (18 of
  20 in review).
- In ci.yml the integration job's `ctest -j` step and the coverage job's `tools/coverage.sh`
  first lift the step shell's limit (`sudo prlimit --pid $$ --memlock=unlimited:unlimited`, from
  #132): one test per core is many processes under one user's counter.
- `tools/io_uring_probe.c` prints the soft limit next to the kernel sysctl.

## Consequences

- No io_uring reactor uses zero-copy sends under a finite locked-memory limit without
  CAP_IPC_LOCK, as in CI's jobs that keep the runners' 8 MiB (build-test, reactor-matrix). That
  includes kernels before 6.15, where `SENDMSG_ZC` cost no locked memory and zero copy was on.
  A process with CAP_IPC_LOCK (root in the initial user namespace) and a finite limit keeps it.
  Hosts and containers with an unlimited limit keep ADR-0051's behaviour, self-hosted runners
  given `LimitMEMLOCK=infinity` as docs/operations/soak.md describes among them;
  `zero_copy_sends` in the datagram stats shows which a process has.
- The integration and coverage steps run unlimited, so there every reactor runs the 1,024-send
  probe and its sockets start on zero copy, switching to plain sends at the first notification
  that the kernel copied, as loopback always does (ADR-0051). Those steps exercise the probe and
  that switch; the unit label (one test at a time, under 8 MiB) exercises the plain path and,
  through the new test, the charge.
- Under an unlimited limit the probe still charges 8 MiB to the user's shared counter while it
  runs; nothing is refused, but the counter is not empty then, so a process of the same user
  under a finite limit can still be refused for that moment.
- Each reactor still charges its user about 416 KiB of locked memory on 6.14 and later, so an
  8 MiB limit holds 19 reactors across all of a user's processes. A process that runs more
  shards than that under one user needs a higher limit; ENOMEM from `make_reactor` names it, and
  `make_reactor_with_fallback` falls back to epoll as ADR-0010 says.
- Ring teardown is asynchronous, so a ring's charge outlives its process by tens of
  milliseconds: processes of one user starting and stopping reactors back to back briefly hold
  more than their live rings.
- Reopen if zero copy is measured to pay for datagrams on a real NIC (ADR-0051), with the limit
  that would need, or if a kernel stops charging rings to RLIMIT_MEMLOCK.
