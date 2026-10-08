# 0094. gateway_server's malloc: one arena and a fixed 128 KiB mmap threshold, not adopted

Status: Rejected
Date: 2026-10-04

## Context

ADR-0081 moved chat_server to jemalloc and left gateway_server on glibc's malloc (2.39). The
gateway's 6 h soak still failed its per-unit memory bound (ADR-0042): on main its RSS settled
at about 30.9 MB and crept by +0.53 MB after the warm-up. This ADR records what the creep was
traced to, the fix that was tried for it, and why that fix was not kept.

**RssAnon, not VmRSS.** The soak judges VmRSS, which adds file-backed and shared pages to the
anonymous ones. File-backed pages fall and rise with the page cache and can hide anonymous
growth, or invent some, so the trace read `RssAnon` from `/proc/<pid>/status` and `smaps`
every minute (`/proc/<pid>/pagemap` in some runs, `malloc_info` every 5 minutes in others),
in local soaks of one build side by side.

**Fragmentation, not a leak.** The live heap stayed flat, about 29 MB in use, while RssAnon
ratcheted, because glibc kept touching pages it had not touched before:

- in the main arena: glibc's mmap threshold is dynamic, so after the first few freed mapped
  chunks the 16–256 KiB transient buffers (S3 and libcurl headers, presigned URLs, playlist
  text, response staging) come from `[heap]`, and each new peak or hole there touches pages;
- in five more arenas, one for each of the four offload threads and one for the health probe's
  thread, over which the same transient buffers spread new pages.

The growth followed time and peaks, not work: 45 minute runs at 4.2 thousand, 2.2 thousand and
127 thousand requests an hour grew +329, +370 and +265 KB/h of RssAnon. Thirty times the work
gave no more growth, so no per-request leak explains it.

Locally, side by side (RssAnon slope ±95% CI, judged after the 15 minute warm-up):

| Run | glibc setting | RssAnon slope |
|---|---|---|
| M, 90 min | defaults | +185 ±8 KB/h |
| C, 60 min | `MALLOC_ARENA_MAX=1` alone | +480 ±69 KB/h, the growth moved to `[heap]` |
| D, 60 min | mmap threshold 128 KiB alone | +285 ±23 KB/h, the growth stayed in the threads' arenas |
| K, 90 min | `MALLOC_ARENA_MAX=1` and mmap threshold 128 KiB | +0 ±10 KB/h |
| L, 90 min | jemalloc 5.3.0 preloaded, decay 0 | −60 ±59 KB/h, VmRSS too noisy for the bound |

Run K looked flat, so #145 proposed `ops::tune_allocator`, called first in the gateway's `run()`
before any thread existed: `mallopt(M_ARENA_MAX, 1)`, `mallopt(M_MMAP_THRESHOLD, 131072)` and
`mallopt(M_TRIM_THRESHOLD, 131072)`, skipped under jemalloc or a sanitizer (LSan detected at
compile time), and skipped whole when the operator set any of the three through `MALLOC_*` or
`GLIBC_TUNABLES`, with the starting line's `"allocator"` field saying which applied. Its
acceptance was the 6 h gateway soak on the self-hosted runner.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| One arena and a fixed 128 KiB mmap and trim threshold, set with `mallopt` at startup (#145) | Run K flat for 90 minutes; no new dependency; every runner of the binary gets it | Rejected: over 6 h it raised the steady RSS by about 19 MB and barely changed the creep (Measurements) |
| The same settings as `GLIBC_TUNABLES`/`MALLOC_*` in the units and manifests | No code | Rejected: the same settings, so the same result, and every way of running the binary must remember them |
| jemalloc for gateway_server too (ADR-0081) | One allocator for both services | Not taken: run L's noise fails the bound's upper end, and it adds a library to the gateway's image |
| `malloc_trim` on a timer | One line | Not taken: one trim by hand left the growth in place; it returns the free top of the heap, not the touched holes inside it (ADR-0081) |
| Keep glibc's defaults, and find out more before changing the allocator | Nothing to undo; main's steady RSS is the lowest measured | Taken |

## Decision

Do not tune glibc's malloc in the gateway. gateway_server keeps glibc's defaults, as on main;
its starting line keeps `"allocator":"default"`. No `mallopt` call, no `ops::tune_allocator`,
and no operator-facing `MALLOC_*` or `GLIBC_TUNABLES` contract is added. transcode_worker keeps
glibc's defaults and chat_server keeps jemalloc (ADR-0081), unchanged.

## Measurements

Two 6 h gateway soaks on the self-hosted runner (`soak-experiment`, `kind=gateway`), both with
the tuning in the binary, against main before it. Bounds are the soak's per-unit ceilings;
per-unit values are the RSS slope's 95% upper end per unit. Both runs are recorded in
docs/operations/soak.md.

| | Main, before | exp/soak-allocator, run 37146904045 | demo/integration with #145, run 37168049376 |
|---|---|---|---|
| Commit | main | 6163f0d | 3b138a3 |
| Gateway RSS, steady | about 30.9 MB | first 49.6 MiB, last 50.0 MiB | first 50.0 MiB, last 50.5 MiB |
| Creep after the warm-up | +0.53 MB | +0.42 MiB | +0.5 MiB |
| Slope over the judged window | | +48.9 KB/h (95% upper end +52.5) | +105.1 KB/h; +5.4 KB/h over the last hour |
| Per request (bound 0.315 B) | | 0.379 B | 0.775 B |
| Per chunk request (bound 0.315 B) | | 8.23 B | 16.8 B |
| Per upload session (bound 39.85 B) | | 45.2 B | 92.4 B |

In both runs the worker's RSS and both processes' descriptors were flat, neither run was
INVALID (every load path ran), and every 5xx was one the soak injects (saturation or a store
fault).

## Why rejected

- **It costs about 19 MB of steady RSS.** The gateway sat at 49.6–50.5 MiB with the tuning,
  against about 30.9 MB on main: one arena and a fixed 128 KiB threshold keep more resident,
  not less.
- **It barely changes the creep.** +0.42 MiB and +0.5 MiB after the warm-up, against +0.53 MB
  on main. Run K's flat 90 minutes of RssAnon did not carry over to 6 h of VmRSS, and both
  tuned runs still fail the per-unit bounds.
- **The creep levels off late in each run.** In the demo/integration run the
  slope over the last hour was +5.4 KB/h, against +105.1 KB/h over the whole window. What is
  left looks like fragmentation settling slowly, not unbounded growth, and the tuning is not
  what settles it.
- What it would have added is not free: one malloc lock shared by the reactor, the four
  offload threads and the probe thread (a microbenchmark put about one reactor malloc in a
  thousand at 20 µs), a startup failure path, and an operator contract for three glibc
  settings.

## Consequences

- gateway_server is as on main: glibc's defaults, `"allocator":"default"` on its starting line,
  no allocator step at startup, and no `MALLOC_*` or `GLIBC_TUNABLES` override in the operator
  contract. `ops::tune_allocator` and its tests are not added.
- The gateway's soak still fails its per-unit memory bound; that is open, and is not settled
  by this ADR.
- An experiment with glibc's settings (ADR-0069's diagnostics) again measures them against the
  shipped defaults, not against settings of the gateway's own.

## Next, for the record only

None of this is done or decided here.

- **An A/B on the same commit and runner.** The runs above differ from main's in commit, branch
  content and time; the demo/integration run carries other branches. One binary, run with and
  without `GLIBC_TUNABLES=glibc.malloc.arena_max=1:glibc.malloc.mmap_threshold=131072:glibc.malloc.trim_threshold=131072`
  on the same runner, would separate the setting from everything else.
- **A heap profile.** heaptrack over a run, or `malloc_info` at 1 h and 6 h, to show which
  arena or bin holds the late growth and whether any of it is live memory.
- **Fixing the soak's judge.** The per-chunk gate divides all of the gateway's growth by chunk
  requests alone, which are about 4.6% of its traffic, so it reads about twenty times the
  per-request figure (8.23 against 0.379 B) and fails on growth that is not per chunk at all.
  The 15 minute warm-up is too short for a heap that is still settling hours in. Changing the
  judge's gates or its warm-up is an owner decision and is not done here.
