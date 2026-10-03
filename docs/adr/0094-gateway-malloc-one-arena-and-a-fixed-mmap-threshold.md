# 0094. gateway_server's malloc: one arena and a fixed 128 KiB mmap threshold

Status: Accepted
Date: 2026-10-03

## Context

ADR-0081 moved chat_server to jemalloc and left gateway_server on glibc's malloc (2.39), whose
soak still failed its memory bound: the gateway grew about as fast on either allocator, so its
growth was traced on its own. What the trace found (local soaks of one build, side by side,
8 clients unless noted, judged after the 15 minute warm-up; `smaps` and `/proc/<pid>/status`
sampled every minute, `/proc/<pid>/pagemap` in some runs, and `malloc_info` every 5 minutes in
runs A–D only):

- The live heap is flat, about 29 MB in use. Resident memory ratchets anyway because glibc keeps
  touching pages it had not touched before, in two places:
  - the main arena. glibc's mmap threshold is dynamic: each freed mapped chunk raises it to that
    chunk's size, so after the first few the 16–256 KiB transient buffers (S3 and libcurl
    headers, presigned URLs, playlist text, response staging) come from `[heap]` instead of a
    mapping of their own, and each new peak or new hole touches pages there;
  - five arenas besides the main one: one for each of the four offload threads and one for the
    health probe's thread. Each thread that allocates gets an arena of its own (64 MiB of
    address space reserved, grown on demand), and the same transient buffers made on those
    threads spread new pages over them.
- The growth follows time and peaks, not work. Runs G, H and I (45 minutes) grew +329, +370
  and +265 KB/h of RssAnon at 4.2 thousand, 2.2 thousand and 127 thousand requests an hour:
  thirty times the work, no more growth. No per-request leak explains it.

Runs K, L, M side by side for 90 minutes (judged on minutes 15–90). Runs B, C, D side by side
for 60 minutes (minutes 15–60), with B at 16 clients. Slopes are RssAnon ±95% CI; the verdict
column is the soak's VmRSS judgment.

| Run | glibc setting | RssAnon slope | Soak verdict (VmRSS) |
|---|---|---|---|
| M, 90 min | defaults | +185 ±8 KB/h | fail |
| C, 60 min | `MALLOC_ARENA_MAX=1` alone | +480 ±69 KB/h: the growth moves to the main arena's `[heap]` | fail |
| D, 60 min | mmap threshold 128 KiB alone (which run had which setting is inferred from its heap and arena growth, not recorded) | +285 ±23 KB/h: the growth stays in the threads' arenas | pass, because its file-backed pages fell over the run and hid the anonymous growth from VmRSS |
| K, 90 min | `MALLOC_ARENA_MAX=1` and mmap threshold 128 KiB | +0 ±10 KB/h | pass |
| B, 60 min, 16 clients | defaults, with one `malloc_trim` by hand at about minute 38 (not a timer) | +326 ±195 KB/h | fail |
| L, 90 min | jemalloc 5.3.0, preloaded, `dirty_decay_ms:0,muzzy_decay_ms:0` | −60 ±59 KB/h, but VmRSS noisy: its 95% upper end over the bound | fail |

The soak's load is paced, not a measure of throughput: about 137,000 requests an hour in K, L
and M alike. Throughput is measured under Consequences.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| One arena and a fixed 128 KiB mmap and trim threshold, set with `mallopt` at startup | Measured flat; no new dependency; every runner of the binary gets it | Accepted |
| jemalloc for gateway_server too (ADR-0081's option, again with decay 0) | One allocator for both services | Rejected: flat on average but its noise fails the bound's upper end, and it adds a library to the gateway's image |
| `malloc_trim` on a timer | One line | Rejected: one trim by hand in run B left the growth in place (+326 ±195 KB/h); it returns the free top of the heap, not the touched holes inside it (also ADR-0081). A timer itself was not measured |
| The mmap threshold alone (ADR-0081 measured 16 KiB for chat) | Fixes the main arena | Rejected: at 128 KiB (run D) the threads' arenas carry on growing. Run E, a 16 KiB threshold alone with one `malloc_trim` by hand at about minute 51, passed (−101 ±212 KB/h of RssAnon over 60 minutes), but its interval is too wide and the trim confounds it: inconclusive |
| `MALLOC_ARENA_MAX=1` alone | Fixes the threads' arenas | Rejected: the growth moves to the main arena |
| The same settings as `GLIBC_TUNABLES`/`MALLOC_ARENA_MAX` in the units and manifests | No code | Rejected: every way of running the binary (systemd, Kubernetes, the soak, a developer's shell) must remember it; the variables remain the operator's override instead |
| Two arenas, to keep the reactor off the offload threads' malloc lock | Less contention | Not taken: end to end one arena cost nothing above the noise; in a microbenchmark one arena's tail per malloc (about 20 µs at p99.9) was roughly halved by two, not removed, and two have no RSS evidence (Consequences) |

## Decision

- `ops::tune_allocator` (`ops/include/ops/allocator.hpp`) calls `mallopt(M_ARENA_MAX, 1)`,
  `mallopt(M_MMAP_THRESHOLD, 128 KiB)` and `mallopt(M_TRIM_THRESHOLD, 128 KiB)`. Setting the
  mmap threshold also turns off glibc's dynamic threshold. A refused `mallopt` is an error
  naming the option and value, and the gateway exits on it (`startup failed`, step
  `allocator`).
- gateway_server calls it first in `run()`, which `main` calls, before any thread exists and
  before anything large is allocated, so every thread shares the main arena.
- Skipped when glibc's malloc is not the allocator: jemalloc found at run time
  (`ops::jemalloc_version()`, linked or preloaded) or a sanitizer build (ASan, TSan, MSan,
  HWASan, LSan, detected at compile time), whose allocator ignores `mallopt` or, LSan's, refuses
  it. Another allocator preloaded in place of glibc's (tcmalloc, say) answers `mallopt` itself,
  with a stub that may report success, so the field can read as tuned for settings it ignores.
- Skipped when the operator set any of the three settings it makes, in either of glibc's
  spellings: `MALLOC_ARENA_MAX`, `MALLOC_MMAP_THRESHOLD_` or `MALLOC_TRIM_THRESHOLD_`, or
  `glibc.malloc.arena_max`, `glibc.malloc.mmap_threshold` or `glibc.malloc.trim_threshold` in
  `GLIBC_TUNABLES`. glibc has applied those before `main`; ours would silently undo them, so
  the whole tuning is left out, not just the one setting. A threshold variable that is present
  but empty counts, since glibc 2.39 reads it as 0 (`elf/dl-tunables.c`); an empty
  `MALLOC_ARENA_MAX` is 0, below its minimum of 1, and glibc ignores it, so it does not. glibc's
  other malloc settings (perturb, check, tcache, top pad, mmap max, arena test) and other
  tunables are not ours, and do not count whichever way they are spelt.
- The starting line's `"allocator"` field says what applies: `"glibc arena_max=1
  mmap_threshold=131072 trim_threshold=131072"`, `"glibc MALLOC_ARENA_MAX=2"` (the operator's
  settings, as set), `"jemalloc"` or `"sanitizer"`. It replaces `"default"`, which said neither.
- transcode_worker keeps glibc's defaults (flat in every soak); chat_server keeps jemalloc
  (ADR-0081).

## Consequences

- One arena puts the reactor, the four offload threads and the probe thread behind one malloc
  lock. End to end it did not show. Measured locally (4 cores shared with other builds, glibc
  2.39), the settings side by side from one build, the default being the same binary with
  `GLIBC_TUNABLES=glibc.malloc.arena_test=8`, which turned the tuning off in that build (it no
  longer does: arena test is not one of the three settings). Besides the soak's load, a probe on
  its own keep-alive connection every 50 ms asked alternately for `/api/v1/healthz`, answered
  on the loop, and an authorised `GET /api/v1/videos/{id}`, a catalog read on an offload
  thread. Probe latency is client-side; the server's is the `request` lines' `ms`, all routes:

  | Run | Setting | Requests/s | healthz p50 / p99 | video p50 / p99 | server p50 / p99 |
  |---|---|---|---|---|---|
  | 30 min, soak load (8 clients) | default | 50.7 | 0.52 / 4.7 ms | 1.30 / 8.5 ms | 2 / 22 ms |
  | | tuned | 50.8 | 0.49 / 4.3 ms | 1.28 / 8.7 ms | 2 / 22 ms |
  | 12 min, 32 clients, no pause | default | 312 | 0.72 / 31.8 ms | 2.54 / 67.5 ms | 38 / 1109 ms |
  | | tuned | 292 | 0.74 / 32.9 ms | 2.55 / 65.6 ms | 52 / 1197 ms |
  | 12 min, 32 clients, no pause, again, three at once | default | 198 | 0.69 / 36.5 ms | 3.52 / 93.0 ms | 85 / 1371 ms |
  | | tuned | 205 | 0.71 / 36.9 ms | 3.50 / 92.3 ms | 68 / 1374 ms |
  | | two arenas, same thresholds | 209 | 0.73 / 39.6 ms | 3.46 / 87.5 ms | 67 / 1334 ms |

  At the soak's load the two are the same. Saturated runs are weak evidence: the host was the
  limit (load average about 14 on 4 cores), and between repeats throughput moved by about 6%
  and the server's p50 by a third, either way.

  So the lock was measured on its own, in a microbenchmark: four threads allocate and free bursts
  of 8–63 blocks of 16 B to 64 KiB (log-uniform, as libpq's and libcurl's buffers), while a fifth,
  standing in for the reactor, times malloc and free of 2–8 KiB pairs with a little work between
  them; 6–8 s a run, three runs each (six at the faster pace), interleaved, at a load average of 1
  to 6. Per pair, in ns:

  | Offload threads | Setting | p50 | p99 | p99.9 | p99.99 |
  |---|---|---|---|---|---|
  | a burst, then 2 ms asleep (about 65,000 allocations a second in all) | default | 72 | 115–123 | 232–252 | 12,800–13,800 |
  | | mmap and trim thresholds only | 73 | 118–126 | 244–301 | 12,200–14,400 |
  | | one arena (with or without the thresholds) | 100–106 | 223–293 | 17,700–23,200 | 48,500–86,400 |
  | | two arenas, with the thresholds | 100–101 | 186–235 | 10,600–19,300 | 52,300–73,400 |
  | a burst, then 0.1 ms asleep (about 600,000 a second) | default | 73 | 122–136 | 310–2,460 | 14,700–34,700 |
  | | one arena, with the thresholds | 110–131 | 15,200–22,900 | 51,100–74,800 | 129,000–344,000 |
  | | two arenas, with the thresholds | 104–117 | 230–21,400 | 25,300–71,600 | 58,800–146,000 |

  The cost is the shared arena, not the thresholds. At about 65,000 offload allocations a second
  (a generous guess; the gateway's own rate was not measured), one arena puts about one malloc in
  a thousand on the reactor thread at 20 µs and one in ten thousand at 50–90 µs; two arenas about
  halve the first at that rate, do no better at the faster one, and barely change the second,
  since five threads still share two arenas. Against request latencies of half a millisecond and
  more (above) that is not material, and two arenas with the thresholds have no RSS evidence (the
  soaks above measured one), so one arena stays. A staging buffer grown 8 KiB at a time to 256
  KiB, which past 128 KiB is now an `mmap` and a `munmap` each time, took about 130 µs at the
  median by default and 160 µs with the thresholds, and 220–270 µs at p99 either way.

  If a profile ever shows `__lll_lock_wait` under `malloc` on the reactor thread,
  `GLIBC_TUNABLES=glibc.malloc.arena_max=2:glibc.malloc.mmap_threshold=131072:glibc.malloc.trim_threshold=131072`
  is the override to try first; it then needs a soak of its own, as the RSS evidence here is for
  one arena (as is the 6 h acceptance soak).
- Buffers of 128 KiB and more are mappings: an `mmap` and a `munmap` each, and their pages
  are faulted in fresh. That is what returns their memory; its cost is inside the measurements
  above.
- The three settings, as `MALLOC_*` variables or `glibc.malloc.*` tunables, still reach the
  gateway, as an override of the whole tuning rather than of one setting, and the starting line
  says so; an experiment with them (ADR-0069's diagnostics) is no longer measuring the shipped
  allocator.
- A soak of a sanitizer build measures the sanitizer's allocator, as before (ADR-0081).
- The acceptance is the 6 h gateway soak on the self-hosted runner (soak-experiment,
  `kind=gateway`); its result is added to docs/operations/soak.md when it has run.
