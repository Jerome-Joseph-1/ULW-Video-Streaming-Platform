# 0048. The ffmpeg child runs under a syscall allowlist

Status: Accepted
Date: 2026-09-29

## Context

`ulw_sandbox` (ADR-0025) confines ffmpeg with namespaces, a read-only root, resource limits and
no capabilities. None of that narrows the kernel interface ffmpeg can reach: a decoder exploit
still has `ptrace`, `keyctl`, `bpf`, `io_uring_setup`, `mount` in a namespace it owns, and every
other call the kernel offers, which is where local privilege escalations live. The pod's
seccomp profile (ADR-0032) is the runtime's default plus three calls, so it filters the worker
and ffmpeg alike and cannot be tighter than the worker needs.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Rely on the pod's profile | Nothing to build | Rejected: it must admit everything the worker, its libraries and the sandbox setup call, a superset of ffmpeg's needs by hundreds of calls |
| libseccomp | A maintained builder for filters | Rejected: the images do not carry it, it would be one more pinned package linked into a helper that deliberately links nothing instrumented, and the filter is a page of code |
| A denylist of dangerous calls | Short, and hard to get wrong for ffmpeg | Rejected: every call added to the kernel later is allowed |
| A hand-written BPF allowlist, installed by the helper in the program's process just before exec | Small, reviewable, tested without a kernel | Accepted |

## Decision

- `infra/ffmpeg/src/seccomp_filter.hpp` builds the program; `ulw_sandbox` installs it in the
  child it forks for the program, after the capabilities are dropped and `no_new_privs` is set,
  which installing needs, and after everything else it does, which the filter would refuse.
  Pid 1 of the namespace, which forks the program, is unfiltered. With no program to run, the
  helper still forks a child and installs the filter in it, so the worker's start-up check
  fails on a host that does not allow it.
- The allowlist is what ffmpeg 6.1.1 and ffprobe called under `strace -f`: the probe and the
  three-rung HLS transcode the worker issues, on h264/aac mp4, vp9/opus webm, mpeg4/mp3 avi,
  hevc/aac and av1 mkv, mpegts, flv, ogg, wmv and prores mov inputs, and on truncated, random
  and empty files. Every run as root gave the same 40 names, and 41 as an ordinary user (below). Of them `ioctl`, `prctl`, `fcntl` and
  `prlimit64` are admitted only with the arguments below, and `clone3` is answered `ENOSYS`, so
  that glibc's threads use `clone`, which is checked too. Seven more are added for what a run
  cannot show without the occasion: `rt_sigreturn` (a handler for SIGTERM, which is how the
  worker stops it), the clocks and sleeps a fallback or a wait reaches, and `gettid`. One is
  from the ordinary-user trace: `geteuid`, which libgcrypt's secure-memory set-up calls after
  `getuid` and `mlock`, but only when not root. It is a read of the caller's own id. `tgkill`
  and `tkill` are added for `SIGABRT` alone (next item).
- Arguments are checked where a call is both needed and dangerous: `ioctl` only for the
  terminal queries `TCGETS` and `TIOCGWINSZ`; `prctl` only `PR_SET_NAME` and
  `PR_CAPBSET_READ`; `fcntl` only descriptor and status flags; `prlimit64` only as a read of
  the caller's own limits; `clone` only as a thread (`CLONE_THREAD`, `CLONE_VM`,
  `CLONE_SIGHAND`) with no `CLONE_NEW*` flag; `tgkill` and `tkill` only with `SIGABRT`. `fork` and `vfork` are not allowed, so ffmpeg
  cannot start a process.
- `abort()` raises `SIGABRT` with `tgkill`, and `av_assert`, glibc's heap checks and
  `__stack_chk_fail` all end in `abort()`. Without the exception they would die of `SIGSYS`
  and read as filter kills. A filter compares registers, not pids, so it cannot restrict the
  target to the caller's own process; `SIGABRT` may be sent to any process of the pid
  namespace, which holds only the helper (pid 1), and a signal sent to pid 1 from inside its
  namespace is dropped unless pid 1 has a handler for it.
- `clone3` answers `ENOSYS`. Its flags are in memory, where a filter cannot read them, and
  glibc falls back to `clone` on that answer. The one other benign errno.
- Everything else, and every call of another ABI (32-bit, x32), is `SECCOMP_RET_KILL_PROCESS`,
  with `SECCOMP_FILTER_FLAG_LOG` so the kill reaches the audit log.
- `execve` is allowed: the helper's child needs it once, and the filter cannot tell that one
  from a later one. What it starts inherits the filter, `no_new_privs` and an empty capability
  set, and the root is read-only.
- A child killed by `SIGSYS` classifies as its own kind, `SyscallBlocked`. It cannot be told
  apart from a gap in this allowlist, which is our bug, not the owner's, so it is neither a
  rejection of the file nor a plain kill: the worker runs it once more, as for a crash, and
  then fails the video with "the decoder was stopped by the sandbox", never "could not be
  decoded". Each occurrence logs `ffmpeg_syscall_blocked_total=N` (the worker's counters are
  log lines) with the job's exit code 159 (128 + `SIGSYS`) above it.
- The filter is per architecture (x86-64 and AArch64 tables; the legacy names `access` and
  `mkdir` are replaced by `faccessat` and `mkdirat` where the architecture has no legacy
  calls). AArch64 is untested here.
- `--no-syscall-filter` leaves it out, for the sandbox's own tests, which run `sh` and `python3`
  as stand-ins. Only the worker builds the helper's arguments, and it never passes it.

## Consequences

- The first version was traced as root only and killed ffprobe on CI, whose runner is an
  ordinary user, on `geteuid`. Nothing had varied the user. Two guards now: the tests run as
  the user CI runs them as (and `AnOrdinaryUsersFfprobeRunsToo` covers it from a root shell),
  and the program's instruction count and hash are pinned by a test, so a compiler or a library
  that built a different filter fails there instead of on a job's transcode.
- The allowlist is judged against the images' ffmpeg and glibc, not the machine it was traced
  on: the worker image pins ffmpeg 7:6.1.1-3ubuntu5 on Ubuntu 24.04 (glibc 2.39), which CI's
  runners install too. The kernel does not enter into it, since the filter sees only what user
  space asks for; a different CPU can, through libraries' feature probes, which is what the
  re-trace on a bump and the tests under both users are for.

- A new ffmpeg or libc that calls something not on the list kills the transcode. The failure
  is `SyscallBlocked`, and the job's video fails after one rerun until the list is extended:
  re-trace on every ffmpeg or base image bump with `tools/trace-ffmpeg-syscalls.sh`, run once as
  root and once as an ordinary user, and add
  what is new. The worker image pins ffmpeg's version for this reason as well.
- The filter costs one comparison per call for the ones early in the table and the kernel's
  cache bitmap for the ones whose verdict never depends on an argument.
- The kernel's audit log, not the worker's, names the call that was killed.
- Reopen if the pipeline moves in-process (no child to filter) or if libseccomp becomes part of
  the image for another reason.
